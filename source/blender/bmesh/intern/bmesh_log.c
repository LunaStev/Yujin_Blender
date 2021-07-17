
/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/** \file
 * \ingroup bmesh
 *
 * The BMLog is an interface for storing undo/redo steps as a BMesh is
 * modified. It only stores changes to the BMesh, not full copies.
 *
 * Currently it supports the following types of changes:
 *
 * - Adding and removing vertices
 * - Adding and removing faces
 * - Moving vertices
 * - Setting vertex paint-mask values
 * - Setting vertex hflags
 */

#include "MEM_guardedalloc.h"

#include "BLI_array.h"
#include "BLI_compiler_attrs.h"
#include "BLI_ghash.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_mempool.h"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_customdata.h"
#include "BKE_mesh.h"

#include "BLI_strict_flags.h"
#include "bmesh.h"
#include "bmesh_log.h"
#include "bmesh_private.h"
#include "range_tree.h"

#define CUSTOMDATA

struct Mesh;

struct BMLogEntry {
  struct BMLogEntry *next, *prev;

  /* The following GHashes map from an element ID to one of the log
   * types above */

  /* Elements that were in the previous entry, but have been
   * deleted */
  GHash *deleted_verts;
  GHash *deleted_faces;

  /* Elements that were not in the previous entry, but are in the
   * result of this entry */
  GHash *added_verts;
  GHash *added_faces;

  /* Vertices whose coordinates, mask value, or hflag have changed */
  GHash *modified_verts;
  GHash *modified_faces;

  BLI_mempool *pool_verts;
  BLI_mempool *pool_faces;

  /* This is only needed for dropping BMLogEntries while still in
   * dynamic-topology mode, as that should release vert/face IDs
   * back to the BMLog but no BMLog pointer is available at that
   * time.
   *
   * This field is not guaranteed to be valid, any use of it should
   * check for NULL. */
  BMLog *log;

  CustomData vdata, edata, ldata, pdata;
  struct BMLogEntry *combined_prev, *combined_next;

  bool fully_copy;  // has full copy
  struct Mesh
      *full_copy_mesh;  // avoid excessive memory use by saving a Mesh instead of copying the bmesh
};

struct BMLog {
  BMLogEntry *frozen_full_mesh;

  int refcount;

  /* Mapping from unique IDs to vertices and faces
   *
   * Each vertex and face in the log gets a unique uinteger
   * assigned. That ID is taken from the set managed by the
   * unused_ids range tree.
   *
   * The ID is needed because element pointers will change as they
   * are created and deleted.
   */

  ThreadRWMutex lock;

  BMesh *bm;

  /* All BMLogEntrys, ordered from earliest to most recent */
  ListBase entries;

  /* The current log entry from entries list
   *
   * If null, then the original mesh from before any of the log
   * entries is current (i.e. there is nothing left to undo.)
   *
   * If equal to the last entry in the entries list, then all log
   * entries have been applied (i.e. there is nothing left to redo.)
   */
  BMLogEntry *current_entry;

  int cd_dyn_vert;
  bool dead;
};

typedef struct {
  float co[3];
  short no[3];
  char hflag;
  float mask;
  void *customdata;
} BMLogVert;

typedef struct {
  uint v_ids[3];
  uint l_ids[3];
  float no[3];
  void *customdata[3];
  void *customdata_f;
  char hflag;
} BMLogFace;

/************************* Get/set element IDs ************************/

/* bypass actual hashing, the keys don't overlap */
#define logkey_hash BLI_ghashutil_inthash_p_simple
#define logkey_cmp BLI_ghashutil_intcmp

static void full_copy_swap(BMesh *bm, BMLog *log, BMLogEntry *entry);
static void full_copy_load(BMesh *bm, BMLog *log, BMLogEntry *entry);
static void bm_log_entry_free(BMLogEntry *entry);
static bool bm_log_free_direct(BMLog *log, bool safe_mode);

static void *log_ghash_lookup(BMLog *log, GHash *gh, const void *key)
{
  BLI_rw_mutex_lock(&log->lock, THREAD_LOCK_READ);
  void *ret = BLI_ghash_lookup(gh, key);
  BLI_rw_mutex_unlock(&log->lock);

  return ret;
}

// this is not 100% threadsafe
static void **log_ghash_lookup_p(BMLog *log, GHash *gh, const void *key)
{
  BLI_rw_mutex_lock(&log->lock, THREAD_LOCK_READ);
  void **ret = BLI_ghash_lookup_p(gh, key);
  BLI_rw_mutex_unlock(&log->lock);

  return ret;
}

static void log_ghash_insert(BMLog *log, GHash *gh, void *key, void *val)
{
  BLI_rw_mutex_lock(&log->lock, THREAD_LOCK_WRITE);
  BLI_ghash_insert(gh, key, val);
  BLI_rw_mutex_unlock(&log->lock);
}

static bool log_ghash_remove(
    BMLog *log, GHash *gh, const void *key, GHashKeyFreeFP keyfree, GHashValFreeFP valfree)
{
  BLI_rw_mutex_lock(&log->lock, THREAD_LOCK_WRITE);
  bool ret = BLI_ghash_remove(gh, key, keyfree, valfree);
  BLI_rw_mutex_unlock(&log->lock);

  return ret;
}

static bool log_ghash_reinsert(
    BMLog *log, GHash *gh, void *key, void *val, GHashKeyFreeFP keyfree, GHashValFreeFP valfree)
{
  BLI_rw_mutex_lock(&log->lock, THREAD_LOCK_WRITE);
  bool ret = BLI_ghash_reinsert(gh, key, val, keyfree, valfree);
  BLI_rw_mutex_unlock(&log->lock);

  return ret;
}

static void bm_log_copy_id(CustomData *cdata, BMElem *elem, void *data)
{
  int cd_id = cdata->typemap[CD_MESH_ID];

  if (cd_id >= 0) {
    cd_id = cdata->layers[cd_id].offset;

    int id = BM_ELEM_CD_GET_INT(elem, cd_id);

    BMElem elem2;
    elem2.head.data = data;

    BM_ELEM_CD_SET_INT(&elem2, cd_id, id);
  }
}

static bool log_ghash_haskey(BMLog *log, GHash *gh, const void *key)
{
  BLI_rw_mutex_lock(&log->lock, THREAD_LOCK_READ);
  bool ret = BLI_ghash_haskey(gh, key);
  BLI_rw_mutex_unlock(&log->lock);

  return ret;
}

static bool log_ghash_ensure_p(BMLog *log, GHash *gh, void *key, void ***val)
{
  BLI_rw_mutex_lock(&log->lock, THREAD_LOCK_WRITE);
  bool ret = BLI_ghash_ensure_p(gh, key, val);
  BLI_rw_mutex_unlock(&log->lock);

  return ret;
}

/* Get the vertex's unique ID from the log */
static uint bm_log_vert_id_get(BMLog *log, BMVert *v)
{
  return (uint)BM_ELEM_GET_ID(log->bm, v);
}

/* Get a vertex from its unique ID */
static BMVert *bm_log_vert_from_id(BMLog *log, uint id)
{
  return id < (uint)log->bm->idmap.map_size ? (BMVert *)log->bm->idmap.map[id] : NULL;
}

BMVert *BM_log_id_vert_get(BMLog *log, uint id)
{
  return bm_log_vert_from_id(log, id);
}

/* Get the face's unique ID from the log */
static uint bm_log_face_id_get(BMLog *log, BMFace *f)
{
  return (uint)BM_ELEM_GET_ID(log->bm, f);
}

uint BM_log_vert_id_get(BMLog *log, BMVert *v)
{
  return bm_log_vert_id_get(log, v);
}

uint BM_log_face_id_get(BMLog *log, BMFace *f)
{
  return bm_log_face_id_get(log, f);
}

/* Get a face from its unique ID */
static BMFace *bm_log_face_from_id(BMLog *log, uint id)
{
  return id < (uint)log->bm->idmap.map_size ? (BMFace *)log->bm->idmap.map[id] : NULL;
}

BMFace *BM_log_id_face_get(BMLog *log, uint id)
{
  return bm_log_face_from_id(log, id);
}

/************************ BMLogVert / BMLogFace ***********************/

static void bm_log_vert_customdata(
    BMesh *bm, BMLog *log, BMLogEntry *entry, BMVert *v, BMLogVert *lv)
{
#ifdef CUSTOMDATA
  // if (!lv) {
  //  return;
  //}

  if (lv->customdata) {
    BLI_mempool_free(entry->vdata.pool, lv->customdata);
    lv->customdata = NULL;
  }

  CustomData_bmesh_copy_data(&bm->vdata, &entry->vdata, v->head.data, &lv->customdata);

  // forcibly copy id
  // bm_log_copy_id(&bm->vdata, (BMElem *)v, lv->customdata);

#endif
}

static void bm_log_face_customdata(BMesh *bm, BMLog *log, BMFace *f, BMLogFace *lf)
{
#ifdef CUSTOMDATA
  BMLogEntry *entry = log->current_entry;

  if (!entry || !lf) {
    printf("bmlog error\n");
    return;
  }

  if (lf->customdata_f) {
    BLI_mempool_free(entry->pdata.pool, lf->customdata_f);
    lf->customdata_f = NULL;
  }

  CustomData_bmesh_copy_data(&bm->pdata, &entry->pdata, f->head.data, &lf->customdata_f);

  // forcibly copy id
  // bm_log_copy_id(&bm->pdata, (BMElem *)f, lf->customdata_f);

  BMLoop *l1 = f->l_first;
  BMLoop *l2 = f->l_first->next;
  BMLoop *l3 = f->l_first->prev;
  BMLoop *ls[3] = {l1, l2, l3};

  for (int i = 0; i < 3; i++) {
    if (lf->customdata[i]) {
      BLI_mempool_free(entry->ldata.pool, lf->customdata[i]);
      lf->customdata[i] = NULL;
    }

    CustomData_bmesh_copy_data(&bm->ldata, &entry->ldata, ls[i]->head.data, &lf->customdata[i]);
  }
#endif
}

/* Get a vertex's paint-mask value
 *
 * Returns zero if no paint-mask layer is present */
static float vert_mask_get(BMVert *v, const int cd_vert_mask_offset)
{
  if (cd_vert_mask_offset != -1) {
    return BM_ELEM_CD_GET_FLOAT(v, cd_vert_mask_offset);
  }
  return 0.0f;
}

/* Set a vertex's paint-mask value
 *
 * Has no effect is no paint-mask layer is present */
static void vert_mask_set(BMVert *v, const float new_mask, const int cd_vert_mask_offset)
{
  if (cd_vert_mask_offset != -1) {
    BM_ELEM_CD_SET_FLOAT(v, cd_vert_mask_offset, new_mask);
  }
}

/* Update a BMLogVert with data from a BMVert */
static void bm_log_vert_bmvert_copy(BMLog *log,
                                    BMLogEntry *entry,
                                    BMLogVert *lv,
                                    BMVert *v,
                                    const int cd_vert_mask_offset,
                                    bool copy_customdata)
{
  copy_v3_v3(lv->co, v->co);
  normal_float_to_short_v3(lv->no, v->no);
  lv->mask = vert_mask_get(v, cd_vert_mask_offset);
  lv->hflag = v->head.hflag;

  if (copy_customdata) {
    bm_log_vert_customdata(log->bm, log, entry, v, lv);
  }
}

/* Allocate and initialize a BMLogVert */
static BMLogVert *bm_log_vert_alloc(BMLog *log,
                                    BMVert *v,
                                    const int cd_vert_mask_offset,
                                    bool log_customdata)
{
  BMLogEntry *entry = log->current_entry;
  BMLogVert *lv = BLI_mempool_alloc(entry->pool_verts);
  lv->customdata = NULL;

  bm_log_vert_bmvert_copy(log, entry, lv, v, cd_vert_mask_offset, log_customdata);

  return lv;
}

/* Allocate and initialize a BMLogFace */
static BMLogFace *bm_log_face_alloc(BMLog *log, BMFace *f)
{
  BMLogEntry *entry = log->current_entry;
  BMLogFace *lf = BLI_mempool_alloc(entry->pool_faces);
  BMVert *v[3];

  lf->customdata_f = lf->customdata[0] = lf->customdata[1] = lf->customdata[2] = NULL;
  BLI_assert(f->len == 3);

  // BM_iter_as_array(NULL, BM_VERTS_OF_FACE, f, (void **)v, 3);
  BM_face_as_array_vert_tri(f, v);

  copy_v3_v3(lf->no, f->no);

  if (log->bm->idmap.flag & BM_LOOP) {
    lf->l_ids[0] = (uint)BM_ELEM_GET_ID(log->bm, f->l_first);
    lf->l_ids[1] = (uint)BM_ELEM_GET_ID(log->bm, f->l_first->next);
    lf->l_ids[2] = (uint)BM_ELEM_GET_ID(log->bm, f->l_first->prev);
  }
  else {
    lf->l_ids[0] = lf->l_ids[1] = lf->l_ids[2] = (uint)-1;
  }

  lf->v_ids[0] = bm_log_vert_id_get(log, v[0]);
  lf->v_ids[1] = bm_log_vert_id_get(log, v[1]);
  lf->v_ids[2] = bm_log_vert_id_get(log, v[2]);

  lf->hflag = f->head.hflag;
  return lf;
}

/************************ Helpers for undo/redo ***********************/

// exec vert kill callbacks before killing faces
static void bm_log_verts_unmake_pre(
    BMesh *bm, BMLog *log, GHash *verts, BMLogEntry *entry, BMLogCallbacks *callbacks)
{
  const int cd_vert_mask_offset = CustomData_get_offset(&bm->vdata, CD_PAINT_MASK);

  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, verts) {
    void *key = BLI_ghashIterator_getKey(&gh_iter);
    BMLogVert *lv = BLI_ghashIterator_getValue(&gh_iter);
    uint id = POINTER_AS_UINT(key);
    BMVert *v = bm_log_vert_from_id(log, id);

    if (!v) {
      printf("bm_log error; vertex id: %p\n", key);
      continue;
    }

    /* Ensure the log has the final values of the vertex before
     * deleting it */
    bm_log_vert_bmvert_copy(log, entry, lv, v, cd_vert_mask_offset, true);

    if (callbacks) {
      callbacks->on_vert_kill(v, callbacks->userdata);
    }
  }
}

static void bm_log_verts_unmake(
    BMesh *bm, BMLog *log, GHash *verts, BMLogEntry *entry, BMLogCallbacks *callbacks)
{
  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, verts) {
    void *key = BLI_ghashIterator_getKey(&gh_iter);
    uint id = POINTER_AS_UINT(key);
    BMVert *v = bm_log_vert_from_id(log, id);

    if (!v) {
      printf("bmlog error.  vertex id: %p\n", key);
      continue;
    }

    BM_vert_kill(bm, v);
  }
}

static void bm_log_faces_unmake(
    BMesh *bm, BMLog *log, GHash *faces, BMLogEntry *entry, BMLogCallbacks *callbacks)
{
  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, faces) {
    void *key = BLI_ghashIterator_getKey(&gh_iter);
    BMLogFace *lf = BLI_ghashIterator_getValue(&gh_iter);
    uint id = POINTER_AS_UINT(key);
    BMFace *f = bm_log_face_from_id(log, id);

    if (!f || f->head.htype != BM_FACE) {
      printf("dyntopo error in %s\n", __func__);
      continue;
    }

    BMEdge *e_tri[3];
    BMLoop *l_iter;
    int i;

    l_iter = BM_FACE_FIRST_LOOP(f);
    for (i = 0; i < 3; i++, l_iter = l_iter->next) {
      e_tri[i] = l_iter->e;
    }

    // ensure we have final customdata for face in log
#ifdef CUSTOMDATA
    if (lf->customdata_f) {
      CustomData_bmesh_copy_data(&bm->pdata, &entry->pdata, f->head.data, &lf->customdata_f);

      // forcibly copy id
      // bm_log_copy_id(&bm->pdata, (BMElem *)f, lf->customdata_f);
    }

    BMLoop *ls[3] = {f->l_first, f->l_first->next, f->l_first->prev};

    for (i = 0; i < 3; i++) {
      if (lf->customdata[i]) {
        CustomData_bmesh_copy_data(
            &bm->ldata, &entry->ldata, ls[i]->head.data, &lf->customdata[i]);
      }
    }
#endif

    if (callbacks) {
      callbacks->on_face_kill(f, callbacks->userdata);
    }

    BM_face_kill(bm, f);

    /* Remove any unused edges */
    for (i = 0; i < 3; i++) {
      if (BM_edge_is_wire(e_tri[i])) {
        BM_edge_kill(bm, e_tri[i]);
      }
    }
  }
}

static void bm_log_verts_restore(
    BMesh *bm, BMLog *log, GHash *verts, BMLogEntry *entry, BMLogCallbacks *callbacks)
{
  const int cd_vert_mask_offset = CustomData_get_offset(&bm->vdata, CD_PAINT_MASK);

  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, verts) {
    void *key = BLI_ghashIterator_getKey(&gh_iter);
    BMLogVert *lv = BLI_ghashIterator_getValue(&gh_iter);
    BMVert *v = BM_vert_create(bm, lv->co, NULL, BM_CREATE_SKIP_ID);
    vert_mask_set(v, lv->mask, cd_vert_mask_offset);
    v->head.hflag = lv->hflag;
    normal_short_to_float_v3(v->no, lv->no);

#ifdef CUSTOMDATA
    if (lv->customdata) {
      CustomData_bmesh_copy_data(&entry->vdata, &bm->vdata, lv->customdata, &v->head.data);
    }
#endif

    bm_assign_id(bm, (BMElem *)v, POINTER_AS_UINT(key));

    if (callbacks) {
      callbacks->on_vert_add(v, callbacks->userdata);
    }
  }
}

static void bm_log_faces_restore(
    BMesh *bm, BMLog *log, GHash *faces, BMLogEntry *entry, BMLogCallbacks *callbacks)
{
  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, faces) {
    void *key = BLI_ghashIterator_getKey(&gh_iter);
    BMLogFace *lf = BLI_ghashIterator_getValue(&gh_iter);
    BMVert *v[3] = {
        bm_log_vert_from_id(log, lf->v_ids[0]),
        bm_log_vert_from_id(log, lf->v_ids[1]),
        bm_log_vert_from_id(log, lf->v_ids[2]),
    };
    BMFace *f;

    if (!v[0] || !v[1] || !v[2]) {
      BMIter iter;
      BMVert *v2;
      const int cd_id = bm->idmap.cd_id_off[BM_VERT];

      BM_ITER_MESH (v2, &iter, bm, BM_VERTS_OF_MESH) {
        int id = BM_ELEM_CD_GET_INT(v2, cd_id);

        for (int i = 0; i < 3; i++) {
          if (!v[i] && lf->v_ids[i] == (uint)id) {
            printf("found vertex\n");
          }
        }
      }
      printf("Undo error! %p %p %p\n", v[0], v[1], v[2]);
      continue;
    }

    f = BM_face_create_verts(bm, v, 3, NULL, BM_CREATE_SKIP_ID, true);
    f->head.hflag = lf->hflag;

    copy_v3_v3(f->no, lf->no);

    if (lf->customdata_f) {
      CustomData_bmesh_copy_data(&entry->pdata, &bm->pdata, lf->customdata_f, &f->head.data);
    }

    bm_assign_id(bm, (BMElem *)f, POINTER_AS_UINT(key));

    BMLoop *ls[3] = {f->l_first, f->l_first->next, f->l_first->prev};

    for (int i = 0; i < 3; i++) {
      if (lf->l_ids[i] != (uint)-1) {
        bm_assign_id(bm, (BMElem *)ls[i], lf->l_ids[i]);
      }

      if (lf->customdata[i]) {
        CustomData_bmesh_copy_data(
            &entry->ldata, &bm->ldata, lf->customdata[i], &ls[i]->head.data);
      }
    }

    if (callbacks) {
      callbacks->on_face_add(f, callbacks->userdata);
    }
  }
}

static void bm_log_vert_values_swap(
    BMesh *bm, BMLog *log, GHash *verts, BMLogEntry *entry, BMLogCallbacks *callbacks)
{
  const int cd_vert_mask_offset = CustomData_get_offset(&bm->vdata, CD_PAINT_MASK);

  void *scratch = bm->vdata.pool ? BLI_mempool_alloc(bm->vdata.pool) : NULL;

  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, verts) {
    void *key = BLI_ghashIterator_getKey(&gh_iter);
    BMLogVert *lv = BLI_ghashIterator_getValue(&gh_iter);
    uint id = POINTER_AS_UINT(key);
    BMVert *v = bm_log_vert_from_id(log, id);
    float mask;
    short normal[3];

    swap_v3_v3(v->co, lv->co);
    copy_v3_v3_short(normal, lv->no);
    normal_float_to_short_v3(lv->no, v->no);
    normal_short_to_float_v3(v->no, normal);
    SWAP(char, v->head.hflag, lv->hflag);
    mask = lv->mask;
    lv->mask = vert_mask_get(v, cd_vert_mask_offset);
    vert_mask_set(v, mask, cd_vert_mask_offset);

    void *old_cdata = NULL;

    if (lv->customdata) {
      if (v->head.data) {
        old_cdata = scratch;
        memcpy(old_cdata, v->head.data, (size_t)bm->vdata.totsize);
      }

      CustomData_bmesh_swap_data(&entry->vdata, &bm->vdata, lv->customdata, &v->head.data);
    }

    if (callbacks) {
      callbacks->on_vert_change(v, callbacks->userdata, old_cdata);
    }
  }

  if (scratch) {
    BLI_mempool_free(bm->vdata.pool, scratch);
  }
}

static void bm_log_face_values_swap(BMLog *log,
                                    GHash *faces,
                                    BMLogEntry *entry,
                                    BMLogCallbacks *callbacks)
{
  void *scratch = log->bm->pdata.pool ? BLI_mempool_alloc(log->bm->pdata.pool) : NULL;

  GHashIterator gh_iter;
  GHASH_ITER (gh_iter, faces) {
    void *key = BLI_ghashIterator_getKey(&gh_iter);
    BMLogFace *lf = BLI_ghashIterator_getValue(&gh_iter);
    uint id = POINTER_AS_UINT(key);
    BMFace *f = bm_log_face_from_id(log, id);

    swap_v3_v3(f->no, lf->no);
    SWAP(char, f->head.hflag, lf->hflag);

    void *old_cdata = NULL;

#ifdef CUSTOMDATA
    if (f->head.data) {
      old_cdata = scratch;
      memcpy(old_cdata, f->head.data, (size_t)log->bm->pdata.totsize);
    }

    if (lf->customdata_f) {
      CustomData_bmesh_swap_data(&entry->pdata, &log->bm->pdata, lf->customdata_f, &f->head.data);
    }

    BMLoop *ls[3] = {f->l_first, f->l_first->next, f->l_first->prev};

    for (int i = 0; i < 3; i++) {
      if (lf->customdata[i]) {
        CustomData_bmesh_swap_data(
            &entry->ldata, &log->bm->ldata, lf->customdata[i], &ls[i]->head.data);
      }
    }
#endif

    if (callbacks) {
      callbacks->on_face_change(f, callbacks->userdata, old_cdata);
    }
  }

  if (scratch) {
    BLI_mempool_free(log->bm->pdata.pool, scratch);
  }
}

/**********************************************************************/

static void bm_log_full_mesh_intern(BMesh *bm, BMLog *log, BMLogEntry *entry)
{
  CustomData_MeshMasks cd_mask_extra = {CD_MASK_DYNTOPO_VERT, 0, 0, 0, 0};

  BM_mesh_elem_index_ensure(bm, BM_VERT | BM_EDGE | BM_FACE);

  entry->full_copy_mesh = BKE_mesh_from_bmesh_nomain(
      bm,
      (&(struct BMeshToMeshParams){.update_shapekey_indices = false,
                                   .calc_object_remap = false,
                                   .cd_mask_extra = cd_mask_extra,
                                   .copy_temp_cdlayers = true,
                                   .copy_mesh_id_layers = true}),
      NULL);
}

/* Allocate an empty log entry */
static BMLogEntry *bm_log_entry_create(void)
{
  BMLogEntry *entry = MEM_callocN(sizeof(BMLogEntry), __func__);

  entry->deleted_verts = BLI_ghash_new(logkey_hash, logkey_cmp, __func__);
  entry->deleted_faces = BLI_ghash_new(logkey_hash, logkey_cmp, __func__);
  entry->added_verts = BLI_ghash_new(logkey_hash, logkey_cmp, __func__);
  entry->added_faces = BLI_ghash_new(logkey_hash, logkey_cmp, __func__);
  entry->modified_verts = BLI_ghash_new(logkey_hash, logkey_cmp, __func__);
  entry->modified_faces = BLI_ghash_new(logkey_hash, logkey_cmp, __func__);

  entry->pool_verts = BLI_mempool_create(sizeof(BMLogVert), 0, 64, BLI_MEMPOOL_NOP);
  entry->pool_faces = BLI_mempool_create(sizeof(BMLogFace), 0, 64, BLI_MEMPOOL_NOP);

  return entry;
}

/* Free the data in a log entry
 *
 * NOTE: does not free the log entry itself. */
static void bm_log_entry_free(BMLogEntry *entry)
{
  BMLog *log = entry->log;
  bool kill_log = false;

  if (log) {
    log->refcount--;

    if (log->refcount < 0) {
      fprintf(stderr, "BMLog refcount error\n");
      log->refcount = 0;
    }

    kill_log = !log->refcount;
  }

  if (entry->full_copy_mesh) {
    BKE_mesh_free(entry->full_copy_mesh);
  }

  BLI_ghash_free(entry->deleted_verts, NULL, NULL);
  BLI_ghash_free(entry->deleted_faces, NULL, NULL);
  BLI_ghash_free(entry->added_verts, NULL, NULL);
  BLI_ghash_free(entry->added_faces, NULL, NULL);
  BLI_ghash_free(entry->modified_verts, NULL, NULL);
  BLI_ghash_free(entry->modified_faces, NULL, NULL);

  BLI_mempool_destroy(entry->pool_verts);
  BLI_mempool_destroy(entry->pool_faces);

  if (entry->vdata.pool) {
    BLI_mempool_destroy(entry->vdata.pool);
  }
  if (entry->edata.pool) {
    BLI_mempool_destroy(entry->edata.pool);
  }
  if (entry->ldata.pool) {
    BLI_mempool_destroy(entry->ldata.pool);
  }
  if (entry->pdata.pool) {
    BLI_mempool_destroy(entry->pdata.pool);
  }

  CustomData_free(&entry->vdata, 0);
  CustomData_free(&entry->edata, 0);
  CustomData_free(&entry->ldata, 0);
  CustomData_free(&entry->pdata, 0);

  if (kill_log) {
    bm_log_free_direct(log, true);
  }
}

static int uint_compare(const void *a_v, const void *b_v)
{
  const uint *a = a_v;
  const uint *b = b_v;
  return (*a) < (*b);
}

/* Remap IDs to contiguous indices
 *
 * E.g. if the vertex IDs are (4, 1, 10, 3), the mapping will be:
 *    4 -> 2
 *    1 -> 0
 *   10 -> 3
 *    3 -> 1
 */
static GHash *bm_log_compress_ids_to_indices(uint *ids, uint totid)
{
  GHash *map = BLI_ghash_int_new_ex(__func__, totid);
  uint i;

  qsort(ids, totid, sizeof(*ids), uint_compare);

  for (i = 0; i < totid; i++) {
    void *key = POINTER_FROM_UINT(ids[i]);
    void *val = POINTER_FROM_UINT(i);
    BLI_ghash_insert(map, key, val);
  }

  return map;
}

/***************************** Public API *****************************/

void BM_log_set_cd_offsets(BMLog *log, int cd_dyn_vert)
{
  log->cd_dyn_vert = cd_dyn_vert;
}

void BM_log_set_bm(BMesh *bm, BMLog *log)
{
  log->bm = bm;
}

/* Allocate, initialize, and assign a new BMLog */
BMLog *BM_log_create(BMesh *bm, int cd_dyn_vert)
{
  BMLog *log = MEM_callocN(sizeof(*log), __func__);

  BLI_rw_mutex_init(&log->lock);

  BM_log_set_cd_offsets(log, cd_dyn_vert);

  return log;
}

void BM_log_cleanup_entry(BMLogEntry *entry)
{
  BMLog *log = entry->log;

  if (log) {
    /* delete entries to avoid releasing ids in node cleanup */
    BLI_ghash_clear(entry->deleted_verts, NULL, NULL);
    BLI_ghash_clear(entry->deleted_faces, NULL, NULL);
    BLI_ghash_clear(entry->added_verts, NULL, NULL);
    BLI_ghash_clear(entry->added_faces, NULL, NULL);
    BLI_ghash_clear(entry->modified_verts, NULL, NULL);
  }
}

BMLog *bm_log_from_existing_entries_create(BMesh *bm,
                                           BMLog *log,
                                           BMLogEntry *entry,
                                           bool restore_ids)
{
  if (entry->prev) {
    log->current_entry = entry;
  }
  else {
    log->current_entry = NULL;
  }

  /* Let BMLog manage the entry list again */
  log->entries.first = log->entries.last = entry;

  {
    while (entry->prev) {
      entry = entry->prev;
      log->entries.first = entry;
    }
    entry = log->entries.last;
    while (entry->next) {
      entry = entry->next;
      log->entries.last = entry;
    }
  }

  for (entry = log->entries.first; entry; entry = entry->next) {
    entry->log = log;
  }

  return log;
}

/* Allocate and initialize a new BMLog using existing BMLogEntries
 *
 * The 'entry' should be the last entry in the BMLog. Its prev pointer
 * will be followed back to find the first entry.
 *
 * The unused IDs field of the log will be initialized by taking all
 * keys from all GHashes in the log entry.
 */
BMLog *BM_log_from_existing_entries_create(BMesh *bm, BMLogEntry *entry)
{
  BMLog *log = BM_log_create(bm, -1);
  bm_log_from_existing_entries_create(bm, log, entry, true);

  return log;
}

BMLog *BM_log_unfreeze(BMesh *bm, BMLogEntry *entry)
{
  if (!entry || !entry->log) {
    return NULL;
  }

  BMLogEntry *frozen = entry->log->frozen_full_mesh;
  if (!frozen && entry->fully_copy) {
    frozen = entry;
  }

  if (!frozen) {
    return entry->log->bm == bm ? entry->log : NULL;
  }

  entry->log->bm = bm;

  full_copy_load(bm, entry->log, frozen);

  if (entry->log->frozen_full_mesh) {
    entry->log->frozen_full_mesh->log = NULL;
    bm_log_entry_free(entry->log->frozen_full_mesh);
    entry->log->frozen_full_mesh = NULL;
  }

  return entry->log;
}

/* Free all the data in a BMLog including the log itself
 * safe_mode means log->refcount will be checked, and if nonzero log will not be freed
 */
static bool bm_log_free_direct(BMLog *log, bool safe_mode)
{
  BMLogEntry *entry;

  if (safe_mode && log->refcount) {
    if (log->frozen_full_mesh) {
      log->frozen_full_mesh->log = NULL;
      bm_log_entry_free(log->frozen_full_mesh);
    }

    log->frozen_full_mesh = bm_log_entry_create();

    bm_log_full_mesh_intern(log->bm, log, log->frozen_full_mesh);

    return false;
  }

  log->dead = true;

  BLI_rw_mutex_end(&log->lock);

  /* Clear the BMLog references within each entry, but do not free
   * the entries themselves */
  for (entry = log->entries.first; entry; entry = entry->next) {
    entry->log = NULL;
  }

  return true;
}

bool BM_log_free(BMLog *log, bool safe_mode)
{
  if (log->dead) {
    MEM_freeN(log);
    return true;
  }

  if (bm_log_free_direct(log, safe_mode)) {
    MEM_freeN(log);
    return true;
  }

  return false;
}

/* Get the number of log entries */
int BM_log_length(const BMLog *log)
{
  return BLI_listbase_count(&log->entries);
}

void BM_log_print_entry(BMLog *log, BMLogEntry *entry)
{
  BMLogEntry *first = entry;

  if (!log) {
    log = entry->log;
  }

  while (first->combined_prev) {
    first = first->combined_prev;
  }

  printf("==bmlog step==\n");

  while (first) {
    if (first->fully_copy) {
      printf(" ==full mesh copy==\n");
    }
    else {
      printf(" ==entry==\n");
      printf("   modified:\n");
      printf("     verts: %d\n", BLI_ghash_len(entry->modified_verts));
      printf("     faces: %d\n", BLI_ghash_len(entry->modified_faces));
      printf("   new:\n");
      printf("     verts: %d\n", BLI_ghash_len(entry->added_verts));
      printf("     faces: %d\n", BLI_ghash_len(entry->added_faces));
      printf("   deleted:\n");
      printf("     verts: %d\n", BLI_ghash_len(entry->deleted_verts));
      printf("     faces: %d\n", BLI_ghash_len(entry->deleted_faces));
      printf("\n");
    }
    printf("\n");
    first = first->combined_next;
  }
}

/* Apply a consistent ordering to BMesh vertices */
void BM_log_mesh_elems_reorder(BMesh *bm, BMLog *log)
{
#if 0  // TODO: make sure no edge cases relying on this function still exist
  uint *varr;
  uint *farr;

  GHash *id_to_idx;

  BMIter bm_iter;
  BMVert *v;
  BMFace *f;

  uint i;

  /* Put all vertex IDs into an array */
  varr = MEM_mallocN(sizeof(int) * (size_t)bm->totvert, __func__);
  BM_ITER_MESH_INDEX (v, &bm_iter, bm, BM_VERTS_OF_MESH, i) {
    varr[i] = bm_log_vert_id_get(log, v);
  }

  /* Put all face IDs into an array */
  farr = MEM_mallocN(sizeof(int) * (size_t)bm->totface, __func__);
  BM_ITER_MESH_INDEX (f, &bm_iter, bm, BM_FACES_OF_MESH, i) {
    farr[i] = bm_log_face_id_get(log, f);
  }

  /* Create BMVert index remap array */
  id_to_idx = bm_log_compress_ids_to_indices(varr, (uint)bm->totvert);
  BM_ITER_MESH_INDEX (v, &bm_iter, bm, BM_VERTS_OF_MESH, i) {
    const uint id = bm_log_vert_id_get(log, v);
    const void *key = POINTER_FROM_UINT(id);
    const void *val = log_ghash_lookup(log, id_to_idx, key);
    varr[i] = POINTER_AS_UINT(val);
  }
  BLI_ghash_free(id_to_idx, NULL, NULL);

  /* Create BMFace index remap array */
  id_to_idx = bm_log_compress_ids_to_indices(farr, (uint)bm->totface);
  BM_ITER_MESH_INDEX (f, &bm_iter, bm, BM_FACES_OF_MESH, i) {
    const uint id = bm_log_face_id_get(log, f);
    const void *key = POINTER_FROM_UINT(id);
    const void *val = log_ghash_lookup(log, id_to_idx, key);
    farr[i] = POINTER_AS_UINT(val);
  }
  BLI_ghash_free(id_to_idx, NULL, NULL);

  BM_mesh_remap(bm, varr, NULL, farr);

  MEM_freeN(varr);
  MEM_freeN(farr);
#endif
}

BMLogEntry *BM_log_entry_check_customdata(BMesh *bm, BMLog *log)
{
  BMLogEntry *entry = log->current_entry;

  if (!entry) {
    printf("no current entry; creating...\n");
    fflush(stdout);
    return BM_log_entry_add_ex(bm, log, false);
  }

#ifndef CUSTOMDATA
  return entry;
#else

  CustomData *cd1[4] = {&bm->vdata, &bm->edata, &bm->ldata, &bm->pdata};
  CustomData *cd2[4] = {&entry->vdata, &entry->edata, &entry->ldata, &entry->pdata};

  for (int i = 0; i < 4; i++) {
    if (!CustomData_layout_is_same(cd1[i], cd2[i])) {
      printf("Customdata changed for undo\n");
      fflush(stdout);
      return BM_log_entry_add_ex(bm, log, true);
    }
  }

  return entry;
#endif
}

/* Start a new log entry and update the log entry list
 *
 * If the log entry list is empty, or if the current log entry is the
 * last entry, the new entry is simply appended to the end.
 *
 * Otherwise, the new entry is added after the current entry and all
 * following entries are deleted.
 *
 * In either case, the new entry is set as the current log entry.
 */
BMLogEntry *BM_log_entry_add(BMesh *bm, BMLog *log)
{
  return BM_log_entry_add_ex(bm, log, false);
}

BMLogEntry *BM_log_entry_add_ex(BMesh *bm, BMLog *log, bool combine_with_last)
{
  if (log->dead) {
    fprintf(stderr, "BMLog Error: log is dead\n");
    fflush(stderr);
    return NULL;
  }

  log->bm = bm;

  /* WARNING: this is now handled by the UndoSystem: BKE_UNDOSYS_TYPE_SCULPT
   * freeing here causes unnecessary complications. */
  BMLogEntry *entry;
#if 0
  /* Delete any entries after the current one */
  entry = log->current_entry;
  if (entry) {
    BMLogEntry *next;
    for (entry = entry->next; entry; entry = next) {
      next = entry->next;
      bm_log_entry_free(entry);
      BLI_freelinkN(&log->entries, entry);
    }
  }
#endif

  /* Create and append the new entry */
  entry = bm_log_entry_create();
  BLI_addtail(&log->entries, entry);
  entry->log = log;

  log->refcount++;

#ifdef CUSTOMDATA
  if (combine_with_last) {
    if (log->current_entry) {
      log->current_entry->combined_next = entry;
      BLI_remlink(&log->entries, log->current_entry);
    }

    entry->combined_prev = log->current_entry;
  }

  CustomData_copy_all_layout(&bm->vdata, &entry->vdata);
  CustomData_copy_all_layout(&bm->edata, &entry->edata);
  CustomData_copy_all_layout(&bm->ldata, &entry->ldata);
  CustomData_copy_all_layout(&bm->pdata, &entry->pdata);

  CustomData_bmesh_init_pool(&entry->vdata, 0, BM_VERT);
  CustomData_bmesh_init_pool(&entry->edata, 0, BM_EDGE);
  CustomData_bmesh_init_pool(&entry->ldata, 0, BM_LOOP);
  CustomData_bmesh_init_pool(&entry->pdata, 0, BM_FACE);
#endif

  log->current_entry = entry;

  return entry;
}

/* Remove an entry from the log
 *
 * Uses entry->log as the log. If the log is NULL, the entry will be
 * free'd but not removed from any list, nor shall its IDs be
 * released.
 *
 * This operation is only valid on the first and last entries in the
 * log. Deleting from the middle will assert.
 */
void BM_log_entry_drop(BMLogEntry *entry)
{
  BMLog *log = entry->log;

  if (!log) {
    /* Unlink */
    BLI_assert(!(entry->prev && entry->next));
    if (entry->prev) {
      entry->prev->next = NULL;
    }
    else if (entry->next) {
      entry->next->prev = NULL;
    }

    bm_log_entry_free(entry);
    MEM_freeN(entry);
    return;
  }

  if (!entry->prev) {
    /* Release IDs of elements that are deleted by this
     * entry. Since the entry is at the beginning of the undo
     * stack, and it's being deleted, those elements can never be
     * restored. Their IDs can go back into the pool. */

    /* This would never happen usually since first entry of log is
     * usually dyntopo enable, which, when reverted will free the log
     * completely. However, it is possible have a stroke instead of
     * dyntopo enable as first entry if nodes have been cleaned up
     * after sculpting on a different object than A, B.
     *
     * The steps are:
     * A dyntopo enable - sculpt
     * B dyntopo enable - sculpt - undo (A objects operators get cleaned up)
     * A sculpt (now A's log has a sculpt operator as first entry)
     *
     * Causing a cleanup at this point will call the code below, however
     * this will invalidate the state of the log since the deleted vertices
     * have been reclaimed already on step 2 (see BM_log_cleanup_entry)
     *
     * Also, design wise, a first entry should not have any deleted vertices since it
     * should not have anything to delete them -from-
     */
    // bm_log_id_ghash_release(log, entry->deleted_faces);
    // bm_log_id_ghash_release(log, entry->deleted_verts);
  }
  else if (!entry->next) {
    /* Release IDs of elements that are added by this entry. Since
     * the entry is at the end of the undo stack, and it's being
     * deleted, those elements can never be restored. Their IDs
     * can go back into the pool. */
  }
  else {
    BLI_assert_msg(0, "Cannot drop BMLogEntry from middle");
  }

  if (log->current_entry == entry) {
    log->current_entry = entry->prev;
  }

  bm_log_entry_free(entry);
  BLI_freelinkN(&log->entries, entry);
}

static void full_copy_load(BMesh *bm, BMLog *log, BMLogEntry *entry)
{
  CustomData_MeshMasks cd_mask_extra = {CD_MASK_DYNTOPO_VERT, 0, 0, 0, 0};

  BM_mesh_clear(bm);
  BM_mesh_bm_from_me(NULL,
                     bm,
                     entry->full_copy_mesh,
                     (&(struct BMeshFromMeshParams){.calc_face_normal = false,
                                                    .add_key_index = false,
                                                    .use_shapekey = false,
                                                    .active_shapekey = -1,

                                                    .cd_mask_extra = cd_mask_extra,
                                                    .copy_temp_cdlayers = true,
                                                    .copy_id_layers = true}));

  bm->elem_index_dirty |= BM_VERT | BM_FACE;

  BM_mesh_elem_table_ensure(bm, BM_VERT | BM_FACE);
  BM_mesh_elem_index_ensure(bm, BM_VERT | BM_FACE);

  bm->elem_index_dirty |= BM_VERT | BM_EDGE | BM_FACE;
  bm->elem_table_dirty |= BM_VERT | BM_EDGE | BM_FACE;
}

static void full_copy_swap(BMesh *bm, BMLog *log, BMLogEntry *entry)
{
  CustomData_MeshMasks cd_mask_extra = {CD_MASK_DYNTOPO_VERT, 0, 0, 0, 0};

  BMLogEntry tmp = {0};

  bm_log_full_mesh_intern(bm, log, &tmp);

  BM_mesh_clear(bm);
  BM_mesh_bm_from_me(NULL,
                     bm,
                     entry->full_copy_mesh,
                     (&(struct BMeshFromMeshParams){.calc_face_normal = false,
                                                    .add_key_index = false,
                                                    .use_shapekey = false,
                                                    .active_shapekey = -1,

                                                    .cd_mask_extra = cd_mask_extra,
                                                    .copy_temp_cdlayers = true,
                                                    .copy_id_layers = true}));

  bm->elem_index_dirty |= BM_VERT | BM_FACE;
  bm->elem_table_dirty |= BM_VERT | BM_FACE;

  BM_mesh_elem_table_ensure(bm, BM_VERT | BM_FACE);
  BM_mesh_elem_index_ensure(bm, BM_VERT | BM_FACE);

  BKE_mesh_free(entry->full_copy_mesh);

  entry->full_copy_mesh = tmp.full_copy_mesh;

  bm->elem_index_dirty |= BM_VERT | BM_EDGE | BM_FACE;
  bm->elem_table_dirty |= BM_VERT | BM_EDGE | BM_FACE;
}

/* Undo one BMLogEntry
 *
 * Has no effect if there's nothing left to undo */
static void bm_log_undo_intern(
    BMesh *bm, BMLog *log, BMLogEntry *entry, BMLogCallbacks *callbacks, const char *node_layer_id)
{
  bm->elem_index_dirty |= BM_VERT | BM_EDGE | BM_FACE;
  bm->elem_table_dirty |= BM_VERT | BM_EDGE | BM_FACE;

  if (entry->fully_copy) {
    full_copy_swap(bm, log, entry);

    if (callbacks) {
      callbacks->on_full_mesh_load(callbacks->userdata);
    }
    return;
  }

  /* Delete added faces and verts */
  bm_log_verts_unmake_pre(bm, log, entry->added_verts, entry, callbacks);
  bm_log_faces_unmake(bm, log, entry->added_faces, entry, callbacks);
  bm_log_verts_unmake(bm, log, entry->added_verts, entry, callbacks);

  /* Restore deleted verts and faces */
  bm_log_verts_restore(bm, log, entry->deleted_verts, entry, callbacks);
  bm_log_faces_restore(bm, log, entry->deleted_faces, entry, callbacks);

  /* Restore vertex coordinates, mask, and hflag */
  bm_log_vert_values_swap(bm, log, entry->modified_verts, entry, callbacks);
  bm_log_face_values_swap(log, entry->modified_faces, entry, callbacks);
}

void BM_log_undo(BMesh *bm, BMLog *log, BMLogCallbacks *callbacks, const char *node_layer_id)
{
  BMLogEntry *entry = log->current_entry;
  log->bm = bm;

  if (!entry) {
    return;
  }

  BMLogEntry *preventry = entry->prev;

  while (entry) {
    bm_log_undo_intern(bm, log, entry, callbacks, node_layer_id);
    entry = entry->combined_prev;
  }

  log->current_entry = preventry;
}

/* Redo one BMLogEntry
 *
 * Has no effect if there's nothing left to redo */
static void bm_log_redo_intern(
    BMesh *bm, BMLog *log, BMLogEntry *entry, BMLogCallbacks *callbacks, const char *node_layer_id)
{
  if (entry->fully_copy) {
    // hrm, should we swap?
    full_copy_swap(bm, log, entry);

    if (callbacks) {
      callbacks->on_full_mesh_load(callbacks->userdata);
    }
    return;
  }

  bm->elem_index_dirty |= BM_VERT | BM_EDGE | BM_FACE;
  bm->elem_table_dirty |= BM_VERT | BM_EDGE | BM_FACE;

  /* Re-delete previously deleted faces and verts */
  bm_log_verts_unmake_pre(bm, log, entry->deleted_verts, entry, callbacks);
  bm_log_faces_unmake(bm, log, entry->deleted_faces, entry, callbacks);
  bm_log_verts_unmake(bm, log, entry->deleted_verts, entry, callbacks);

  /* Restore previously added verts and faces */
  bm_log_verts_restore(bm, log, entry->added_verts, entry, callbacks);
  bm_log_faces_restore(bm, log, entry->added_faces, entry, callbacks);

  /* Restore vertex coordinates, mask, and hflag */
  bm_log_vert_values_swap(bm, log, entry->modified_verts, entry, callbacks);
  bm_log_face_values_swap(log, entry->modified_faces, entry, callbacks);
}

void BM_log_redo(BMesh *bm, BMLog *log, BMLogCallbacks *callbacks, const char *node_layer_id)
{
  BMLogEntry *entry = log->current_entry;
  log->bm = bm;

  if (!entry) {
    /* Currently at the beginning of the undo stack, move to first entry */
    entry = log->entries.first;
  }
  else if (entry->next) {
    /* Move to next undo entry */
    entry = entry->next;
  }

  if (!entry) {
    /* Currently at the end of the undo stack, nothing left to redo */
    return;
  }

  BMLogEntry *nextentry = entry;

  while (entry->combined_prev) {
    entry = entry->combined_prev;
  }

  while (entry) {
    bm_log_redo_intern(bm, log, entry, callbacks, node_layer_id);
    entry = entry->combined_next;
  }

  log->current_entry = nextentry;
}
/* Log a vertex before it is modified
 *
 * Before modifying vertex coordinates, masks, or hflags, call this
 * function to log its current values. This is better than logging
 * after the coordinates have been modified, because only those
 * vertices that are modified need to have their original values
 * stored.
 *
 * Handles two separate cases:
 *
 * If the vertex was added in the current log entry, update the
 * vertex in the map of added vertices.
 *
 * If the vertex already existed prior to the current log entry, a
 * separate key/value map of modified vertices is used (using the
 * vertex's ID as the key). The values stored in that case are
 * the vertex's original state so that an undo can restore the
 * previous state.
 *
 * On undo, the current vertex state will be swapped with the stored
 * state so that a subsequent redo operation will restore the newer
 * vertex state.
 */
void BM_log_vert_before_modified(BMLog *log,
                                 BMVert *v,
                                 const int cd_vert_mask_offset,
                                 bool log_customdata)
{
  BMLogEntry *entry = log->current_entry;
  BMLogVert *lv;
  uint v_id = (uint)BM_ELEM_GET_ID(log->bm, v);
  void *key = POINTER_FROM_UINT(v_id);
  void **val_p;

  /* Find or create the BMLogVert entry */
  if ((lv = log_ghash_lookup(log, entry->added_verts, key))) {
    bm_log_vert_bmvert_copy(log, entry, lv, v, cd_vert_mask_offset, log_customdata);
  }
  else if (!log_ghash_ensure_p(log, entry->modified_verts, key, &val_p)) {
    lv = bm_log_vert_alloc(log, v, cd_vert_mask_offset, true);
    *val_p = lv;
  }
}

/* Log a new vertex as added to the BMesh
 *
 * The new vertex gets a unique ID assigned. It is then added to a map
 * of added vertices, with the key being its ID and the value
 * containing everything needed to reconstruct that vertex.
 */
void BM_log_vert_added(BMLog *log, BMVert *v, const int cd_vert_mask_offset)
{
  BMLogVert *lv;
  uint v_id = (uint)BM_ELEM_GET_ID(log->bm, v);
  void *key = POINTER_FROM_UINT(v_id);

  lv = bm_log_vert_alloc(log, v, cd_vert_mask_offset, true);
  log_ghash_insert(log, log->current_entry->added_verts, key, lv);
}

/* Log a face before it is modified
 *
 * We always assume face has been added before
 */
void BM_log_face_modified(BMLog *log, BMFace *f)
{
  BMLogFace *lf;
  uint f_id = (uint)BM_ELEM_GET_ID(log->bm, f);
  void *key = POINTER_FROM_UINT(f_id);

  lf = bm_log_face_alloc(log, f);

  log_ghash_insert(log, log->current_entry->modified_faces, key, lf);
  bm_log_face_customdata(log->bm, log, f, lf);
}

/* Log a new face as added to the BMesh
 *
 * The new face gets a unique ID assigned. It is then added to a map
 * of added faces, with the key being its ID and the value containing
 * everything needed to reconstruct that face.
 */
void BM_log_face_added(BMLog *log, BMFace *f)
{
  BMLogFace *lf;
  uint f_id = (uint)BM_ELEM_GET_ID(log->bm, f);
  void *key = POINTER_FROM_UINT(f_id);

  /* Only triangles are supported for now */
  BLI_assert(f->len == 3);

  lf = bm_log_face_alloc(log, f);
  log_ghash_insert(log, log->current_entry->added_faces, key, lf);

  bm_log_face_customdata(log->bm, log, f, lf);
}

/* Log a vertex as removed from the BMesh
 *
 * A couple things can happen here:
 *
 * If the vertex was added as part of the current log entry, then it's
 * deleted and forgotten about entirely. Its unique ID is returned to
 * the unused pool.
 *
 * If the vertex was already part of the BMesh before the current log
 * entry, it is added to a map of deleted vertices, with the key being
 * its ID and the value containing everything needed to reconstruct
 * that vertex.
 *
 * If there's a move record for the vertex, that's used as the
 * vertices original location, then the move record is deleted.
 */
void BM_log_vert_removed(BMLog *log, BMVert *v, const int cd_vert_mask_offset)
{
  BMLogEntry *entry = log->current_entry;
  uint v_id = (uint)BM_ELEM_GET_ID(log->bm, v);
  void *key = POINTER_FROM_UINT(v_id);

  /* if it has a key, it shouldn't be NULL */
  BLI_assert(!!log_ghash_lookup(log, entry->added_verts, key) ==
             !!log_ghash_haskey(log, entry->added_verts, key));

  if (!log_ghash_remove(log, entry->added_verts, key, NULL, NULL)) {
    BMLogVert *lv, *lv_mod;

    lv = bm_log_vert_alloc(log, v, cd_vert_mask_offset, false);
    log_ghash_insert(log, entry->deleted_verts, key, lv);

    /* If the vertex was modified before deletion, ensure that the
     * original vertex values are stored */
    if ((lv_mod = log_ghash_lookup(log, entry->modified_verts, key))) {
      if (lv->customdata) {
        BLI_mempool_free(entry->vdata.pool, lv->customdata);
      }

      (*lv) = (*lv_mod);
      lv_mod->customdata = NULL;

      log_ghash_remove(log, entry->modified_verts, key, NULL, NULL);
      BLI_mempool_free(entry->pool_verts, lv_mod);
    }
    else {
      bm_log_vert_customdata(log->bm, log, entry, v, lv);
    }
  }
}

/* Log a face as removed from the BMesh
 *
 * A couple things can happen here:
 *
 * If the face was added as part of the current log entry, then it's
 * deleted and forgotten about entirely. Its unique ID is returned to
 * the unused pool.
 *
 * If the face was already part of the BMesh before the current log
 * entry, it is added to a map of deleted faces, with the key being
 * its ID and the value containing everything needed to reconstruct
 * that face.
 */
void BM_log_face_removed(BMLog *log, BMFace *f)
{
  BMLogEntry *entry = log->current_entry;
  uint f_id = (uint)BM_ELEM_GET_ID(log->bm, f);
  void *key = POINTER_FROM_UINT(f_id);

  /* if it has a key, it shouldn't be NULL */
  BLI_assert(!!log_ghash_lookup(log, entry->added_faces, key) ==
             !!log_ghash_haskey(log, entry->added_faces, key));

  if (!log_ghash_remove(log, entry->added_faces, key, NULL, NULL)) {
    BMLogFace *lf;

    lf = bm_log_face_alloc(log, f);
    log_ghash_insert(log, entry->deleted_faces, key, lf);

    if (lf) {
      bm_log_face_customdata(log->bm, log, f, lf);
    }
  }
}

/* Log all vertices/faces in the BMesh as added */
void BM_log_all_added(BMesh *bm, BMLog *log)
{
  if (!log->current_entry) {
    BM_log_entry_add_ex(bm, log, false);
  }

  const int cd_vert_mask_offset = CustomData_get_offset(&bm->vdata, CD_PAINT_MASK);
  BMIter bm_iter;
  BMVert *v;
  BMFace *f;

  /* avoid unnecessary resizing on initialization */
  if (BLI_ghash_len(log->current_entry->added_verts) == 0) {
    BLI_ghash_reserve(log->current_entry->added_verts, (uint)bm->totvert);
  }

  if (BLI_ghash_len(log->current_entry->added_faces) == 0) {
    BLI_ghash_reserve(log->current_entry->added_faces, (uint)bm->totface);
  }

  /* Log all vertices as newly created */
  BM_ITER_MESH (v, &bm_iter, bm, BM_VERTS_OF_MESH) {
    BM_log_vert_added(log, v, cd_vert_mask_offset);
  }

  /* Log all faces as newly created */
  BM_ITER_MESH (f, &bm_iter, bm, BM_FACES_OF_MESH) {
    BM_log_face_added(log, f);
  }
}

void BM_log_full_mesh(BMesh *bm, BMLog *log)
{
  BMLogEntry *entry = log->current_entry;

  if (!entry) {
    entry = BM_log_entry_add_ex(bm, log, false);
  }

  bool add = BLI_ghash_len(entry->added_faces) > 0;
  add |= BLI_ghash_len(entry->modified_verts) > 0;
  add |= BLI_ghash_len(entry->modified_faces) > 0;
  add |= BLI_ghash_len(entry->deleted_verts) > 0;
  add |= BLI_ghash_len(entry->deleted_faces) > 0;

  if (add) {
    entry = BM_log_entry_add_ex(bm, log, true);
  }

  entry->fully_copy = true;

  bm_log_full_mesh_intern(bm, log, entry);

  // push a fresh entry
  BM_log_entry_add_ex(bm, log, true);
}

/* Log all vertices/faces in the BMesh as removed */
void BM_log_before_all_removed(BMesh *bm, BMLog *log)
{
  if (!log->current_entry) {
    BM_log_entry_add_ex(bm, log, false);
  }

  const int cd_vert_mask_offset = CustomData_get_offset(&bm->vdata, CD_PAINT_MASK);
  BMIter bm_iter;
  BMVert *v;
  BMFace *f;

  /* Log deletion of all faces */
  BM_ITER_MESH (f, &bm_iter, bm, BM_FACES_OF_MESH) {
    BM_log_face_removed(log, f);
  }

  /* Log deletion of all vertices */
  BM_ITER_MESH (v, &bm_iter, bm, BM_VERTS_OF_MESH) {
    BM_log_vert_removed(log, v, cd_vert_mask_offset);
  }
}

/* Get the logged coordinates of a vertex
 *
 * Does not modify the log or the vertex */
const float *BM_log_original_vert_co(BMLog *log, BMVert *v)
{
  BMLogEntry *entry = log->current_entry;
  const BMLogVert *lv;
  uint v_id = (uint)BM_ELEM_GET_ID(log->bm, v);
  void *key = POINTER_FROM_UINT(v_id);

  BLI_assert(entry);

  BLI_assert(log_ghash_haskey(log, entry->modified_verts, key));

  lv = log_ghash_lookup(log, entry->modified_verts, key);
  return lv->co;
}

/* Get the logged normal of a vertex
 *
 * Does not modify the log or the vertex */
const short *BM_log_original_vert_no(BMLog *log, BMVert *v)
{
  BMLogEntry *entry = log->current_entry;
  const BMLogVert *lv;
  uint v_id = (uint)BM_ELEM_GET_ID(log->bm, v);
  void *key = POINTER_FROM_UINT(v_id);

  BLI_assert(entry);

  BLI_assert(log_ghash_haskey(log, entry->modified_verts, key));

  lv = log_ghash_lookup(log, entry->modified_verts, key);
  return lv->no;
}

/* Get the logged mask of a vertex
 *
 * Does not modify the log or the vertex */
float BM_log_original_mask(BMLog *log, BMVert *v)
{
  BMLogEntry *entry = log->current_entry;
  const BMLogVert *lv;
  uint v_id = (uint)BM_ELEM_GET_ID(log->bm, v);
  void *key = POINTER_FROM_UINT(v_id);

  BLI_assert(entry);

  BLI_assert(log_ghash_haskey(log, entry->modified_verts, key));

  lv = log_ghash_lookup(log, entry->modified_verts, key);
  return lv->mask;
}

void BM_log_original_vert_data(BMLog *log, BMVert *v, const float **r_co, const short **r_no)
{
  BMLogEntry *entry = log->current_entry;
  const BMLogVert *lv;
  uint v_id = (uint)BM_ELEM_GET_ID(log->bm, v);
  void *key = POINTER_FROM_UINT(v_id);

  BLI_assert(entry);

  BLI_assert(log_ghash_haskey(log, entry->modified_verts, key));

  lv = log_ghash_lookup(log, entry->modified_verts, key);
  *r_co = lv->co;
  *r_no = lv->no;
}

/************************ Debugging and Testing ***********************/

/* For internal use only (unit testing) */
BMLogEntry *BM_log_current_entry(BMLog *log)
{
  return log->current_entry;
}

#if 0
/* Print the list of entries, marking the current one
 *
 * Keep around for debugging */
void bm_log_print(const BMLog *log, const char *description)
{
  const BMLogEntry *entry;
  const char *current = " <-- current";
  int i;

  printf("%s:\n", description);
  printf("    % 2d: [ initial ]%s\n", 0, (!log->current_entry) ? current : "");
  for (entry = log->entries.first, i = 1; entry; entry = entry->next, i++) {
    printf("    % 2d: [%p]%s\n", i, entry, (entry == log->current_entry) ? current : "");
  }
}
#endif
