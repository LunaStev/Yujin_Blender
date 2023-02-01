/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "AS_asset_library.hh"

#include "BKE_context.h"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "ED_asset.h"

#include "UI_interface.h"
#include "UI_resources.h"
#include "interface_intern.hh"

#include "RNA_prototypes.h"

using namespace blender;

/* TODO copy of #asset_view_item_but_drag_set(). */
static void asset_tile_but_drag_set(uiBut &but, AssetHandle &asset_handle)
{
  ID *id = ED_asset_handle_get_local_id(&asset_handle);
  if (id != nullptr) {
    UI_but_drag_set_id(&but, id);
    return;
  }

  char blend_path[FILE_MAX_LIBEXTRA];
  /* Context can be null here, it's only needed for a File Browser specific hack that should go
   * away before too long. */
  ED_asset_handle_get_full_library_path(&asset_handle, blend_path);

  if (blend_path[0]) {
    ImBuf *imbuf = ED_assetlist_asset_image_get(&asset_handle);
    UI_but_drag_set_asset(&but,
                          &asset_handle,
                          BLI_strdup(blend_path),
                          FILE_ASSET_IMPORT_APPEND,
                          ED_asset_handle_get_preview_icon_id(&asset_handle),
                          imbuf,
                          1.0f);
  }
}

static void asset_tile_draw(uiLayout &layout,
                            AssetHandle &asset_handle,
                            const int width,
                            const int height,
                            const bool show_names)
{
  uiBlock *block = uiLayoutGetBlock(&layout);
  uiBut *but = uiDefIconTextBut(block,
                                UI_BTYPE_PREVIEW_TILE,
                                0,
                                ED_asset_handle_get_preview_icon_id(&asset_handle),
                                show_names ? ED_asset_handle_get_name(&asset_handle) : "",
                                0,
                                0,
                                width,
                                height,
                                nullptr,
                                0,
                                0,
                                0,
                                0,
                                "");
  ui_def_but_icon(but,
                  ED_asset_handle_get_preview_icon_id(&asset_handle),
                  /* NOLINTNEXTLINE: bugprone-suspicious-enum-usage */
                  UI_HAS_ICON | UI_BUT_ICON_PREVIEW);
  asset_tile_but_drag_set(*but, asset_handle);
}

static std::optional<asset_system::AssetCatalogFilter> catalog_filter_from_shelf_settings(
    const AssetShelfSettings *shelf_settings, const asset_system::AssetLibrary *library)
{
  if (!shelf_settings || !shelf_settings->active_catalog_path) {
    return {};
  }

  asset_system ::AssetCatalog *active_catalog = library->catalog_service->find_catalog_by_path(
      shelf_settings->active_catalog_path);
  if (!active_catalog) {
    return {};
  }

  return library->catalog_service->create_catalog_filter(active_catalog->catalog_id);
}

void uiTemplateAssetShelf(uiLayout *layout,
                          const bContext *C,
                          const AssetFilterSettings *filter_settings)
{
  const AssetLibraryReference *library_ref = CTX_wm_asset_library_ref(C);
  const PointerRNA shelf_settings_ptr = CTX_data_pointer_get_type(
      C, "asset_shelf_settings", &RNA_AssetShelfSettings);
  const AssetShelfSettings *shelf_settings = static_cast<AssetShelfSettings *>(
      shelf_settings_ptr.data);

  ED_assetlist_storage_fetch(library_ref, C);
  ED_assetlist_ensure_previews_job(library_ref, C);

  const asset_system::AssetLibrary *library = ED_assetlist_library_get_once_available(
      *library_ref);
  if (!library) {
    return;
  }

  std::optional<asset_system::AssetCatalogFilter> catalog_filter =
      catalog_filter_from_shelf_settings(shelf_settings, library);

  uiLayoutSetScaleX(layout, 1.0f);
  uiLayoutSetScaleY(layout, 1.0f);

  const bool show_names = true;
  const int height = uiLayoutGetRootHeight(layout) - UI_style_get_dpi()->boxspace * 2;
  /* Width is derived from the height. It's the height without the space for the name (if there is
   * any). */
  const int width = height - (show_names ? 0 : UI_UNIT_Y);

  uiLayout *box = uiLayoutBox(layout);
  uiLayout *row = uiLayoutRow(box, false);

  ED_assetlist_iterate(*library_ref, [&](AssetHandle asset) {
    if (!ED_asset_filter_matches_asset(filter_settings, &asset)) {
      /* Don't do anything else, but return true to continue iterating. */
      return true;
    }
    /* Filter by active catalog. */
    const AssetMetaData *asset_data = ED_asset_handle_get_metadata(&asset);
    if (catalog_filter && !catalog_filter->contains(asset_data->catalog_id)) {
      return true;
    }

    asset_tile_draw(*row, asset, width, height, show_names);
    return true;
  });
}
