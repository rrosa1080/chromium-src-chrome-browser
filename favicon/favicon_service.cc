// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/favicon/favicon_service.h"

#include <cmath>

#include "base/hash.h"
#include "base/message_loop/message_loop_proxy.h"
#include "chrome/browser/history/history_backend.h"
#include "chrome/browser/history/history_service.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/ui/webui/chrome_web_ui_controller_factory.h"
#include "chrome/common/importer/imported_favicon_usage.h"
#include "chrome/common/url_constants.h"
#include "components/favicon_base/favicon_types.h"
#include "components/favicon_base/favicon_util.h"
#include "components/favicon_base/select_favicon_frames.h"
#include "extensions/common/constants.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/favicon_size.h"
#include "ui/gfx/image/image_skia.h"

using base::Bind;

namespace {

void CancelOrRunFaviconResultsCallback(
    const base::CancelableTaskTracker::IsCanceledCallback& is_canceled,
    const favicon_base::FaviconResultsCallback& callback,
    const std::vector<favicon_base::FaviconRawBitmapResult>& results) {
  if (is_canceled.Run())
    return;
  callback.Run(results);
}

// Helper to run callback with empty results if we cannot get the history
// service.
base::CancelableTaskTracker::TaskId RunWithEmptyResultAsync(
    const favicon_base::FaviconResultsCallback& callback,
    base::CancelableTaskTracker* tracker) {
  return tracker->PostTask(
      base::MessageLoopProxy::current().get(),
      FROM_HERE,
      Bind(callback, std::vector<favicon_base::FaviconRawBitmapResult>()));
}

// Return the TaskId to retreive the favicon from chrome specific URL.
base::CancelableTaskTracker::TaskId GetFaviconForChromeURL(
    Profile* profile,
    const GURL& page_url,
    const std::vector<int>& desired_sizes_in_pixel,
    const favicon_base::FaviconResultsCallback& callback,
    base::CancelableTaskTracker* tracker) {
  base::CancelableTaskTracker::IsCanceledCallback is_canceled_cb;
  base::CancelableTaskTracker::TaskId id =
      tracker->NewTrackedTaskId(&is_canceled_cb);
  favicon_base::FaviconResultsCallback cancelable_cb =
      Bind(&CancelOrRunFaviconResultsCallback, is_canceled_cb, callback);
  ChromeWebUIControllerFactory::GetInstance()->GetFaviconForURL(
      profile, page_url, desired_sizes_in_pixel, cancelable_cb);
  return id;
}

// Returns a vector of pixel edge sizes from |size_in_dip| and
// favicon_base::GetFaviconScales().
std::vector<int> GetPixelSizesForFaviconScales(int size_in_dip) {
  std::vector<float> scales = favicon_base::GetFaviconScales();
  std::vector<int> sizes_in_pixel;
  for (size_t i = 0; i < scales.size(); ++i) {
    sizes_in_pixel.push_back(std::ceil(size_in_dip * scales[i]));
  }
  return sizes_in_pixel;
}

}  // namespace

FaviconService::FaviconService(Profile* profile)
    : history_service_(HistoryServiceFactory::GetForProfile(
          profile, Profile::EXPLICIT_ACCESS)),
      profile_(profile) {
}

// static
void FaviconService::FaviconResultsCallbackRunner(
    const favicon_base::FaviconResultsCallback& callback,
    const std::vector<favicon_base::FaviconRawBitmapResult>* results) {
  callback.Run(*results);
}

base::CancelableTaskTracker::TaskId FaviconService::GetFaviconImage(
    const GURL& icon_url,
    favicon_base::IconType icon_type,
    int desired_size_in_dip,
    const favicon_base::FaviconImageCallback& callback,
    base::CancelableTaskTracker* tracker) {
  favicon_base::FaviconResultsCallback callback_runner =
      Bind(&FaviconService::RunFaviconImageCallbackWithBitmapResults,
           base::Unretained(this), callback, desired_size_in_dip);
  if (history_service_) {
    std::vector<GURL> icon_urls;
    icon_urls.push_back(icon_url);
    return history_service_->GetFavicons(
        icon_urls,
        icon_type,
        GetPixelSizesForFaviconScales(desired_size_in_dip),
        callback_runner,
        tracker);
  }
  return RunWithEmptyResultAsync(callback_runner, tracker);
}

base::CancelableTaskTracker::TaskId FaviconService::GetRawFavicon(
    const GURL& icon_url,
    favicon_base::IconType icon_type,
    int desired_size_in_dip,
    float desired_favicon_scale,
    const favicon_base::FaviconRawBitmapCallback& callback,
    base::CancelableTaskTracker* tracker) {
  int desired_size_in_pixel =
      std::ceil(desired_size_in_dip * desired_favicon_scale);
  favicon_base::FaviconResultsCallback callback_runner =
      Bind(&FaviconService::RunFaviconRawBitmapCallbackWithBitmapResults,
           base::Unretained(this),
           callback,
           desired_size_in_pixel);

  if (history_service_) {
    std::vector<GURL> icon_urls;
    icon_urls.push_back(icon_url);
    std::vector<int> desired_sizes_in_pixel;
    desired_sizes_in_pixel.push_back(desired_size_in_pixel);

    return history_service_->GetFavicons(
        icon_urls, icon_type, desired_sizes_in_pixel, callback_runner, tracker);
  }
  return RunWithEmptyResultAsync(callback_runner, tracker);
}

base::CancelableTaskTracker::TaskId FaviconService::GetFavicon(
    const GURL& icon_url,
    favicon_base::IconType icon_type,
    int desired_size_in_dip,
    const favicon_base::FaviconResultsCallback& callback,
    base::CancelableTaskTracker* tracker) {
  if (history_service_) {
    std::vector<GURL> icon_urls;
    icon_urls.push_back(icon_url);
    return history_service_->GetFavicons(
        icon_urls,
        icon_type,
        GetPixelSizesForFaviconScales(desired_size_in_dip),
        callback,
        tracker);
  }
  return RunWithEmptyResultAsync(callback, tracker);
}

base::CancelableTaskTracker::TaskId
FaviconService::UpdateFaviconMappingsAndFetch(
    const GURL& page_url,
    const std::vector<GURL>& icon_urls,
    int icon_types,
    int desired_size_in_dip,
    const favicon_base::FaviconResultsCallback& callback,
    base::CancelableTaskTracker* tracker) {
  if (history_service_) {
    return history_service_->UpdateFaviconMappingsAndFetch(
        page_url,
        icon_urls,
        icon_types,
        GetPixelSizesForFaviconScales(desired_size_in_dip),
        callback,
        tracker);
  }
  return RunWithEmptyResultAsync(callback, tracker);
}

base::CancelableTaskTracker::TaskId FaviconService::GetFaviconImageForPageURL(
    const FaviconForPageURLParams& params,
    const favicon_base::FaviconImageCallback& callback,
    base::CancelableTaskTracker* tracker) {
  return GetFaviconForPageURLImpl(
      params,
      GetPixelSizesForFaviconScales(params.desired_size_in_dip),
      Bind(&FaviconService::RunFaviconImageCallbackWithBitmapResults,
           base::Unretained(this),
           callback,
           params.desired_size_in_dip),
      tracker);
}

base::CancelableTaskTracker::TaskId FaviconService::GetRawFaviconForPageURL(
    const FaviconForPageURLParams& params,
    float desired_favicon_scale,
    const favicon_base::FaviconRawBitmapCallback& callback,
    base::CancelableTaskTracker* tracker) {
  int desired_size_in_pixel =
      std::ceil(params.desired_size_in_dip * desired_favicon_scale);
  std::vector<int> desired_sizes_in_pixel;
  desired_sizes_in_pixel.push_back(desired_size_in_pixel);
  return GetFaviconForPageURLImpl(
      params,
      desired_sizes_in_pixel,
      Bind(&FaviconService::RunFaviconRawBitmapCallbackWithBitmapResults,
           base::Unretained(this),
           callback,
           desired_size_in_pixel),
      tracker);
}

base::CancelableTaskTracker::TaskId
FaviconService::GetLargestRawFaviconForPageURL(
    Profile* profile,
    const GURL& page_url,
    const std::vector<int>& icon_types,
    int minimum_size_in_pixels,
    const favicon_base::FaviconRawBitmapCallback& callback,
    base::CancelableTaskTracker* tracker) {
  favicon_base::FaviconResultsCallback favicon_results_callback =
      Bind(&FaviconService::RunFaviconRawBitmapCallbackWithBitmapResults,
           base::Unretained(this),
           callback,
           0);
  if (page_url.SchemeIs(content::kChromeUIScheme) ||
      page_url.SchemeIs(extensions::kExtensionScheme)) {
    std::vector<int> desired_sizes_in_pixel;
    desired_sizes_in_pixel.push_back(0);
    return GetFaviconForChromeURL(profile,
                                  page_url,
                                  desired_sizes_in_pixel,
                                  favicon_results_callback,
                                  tracker);
  }
  if (history_service_) {
    return history_service_->GetLargestFaviconForURL(page_url, icon_types,
        minimum_size_in_pixels, callback, tracker);
  }
  return RunWithEmptyResultAsync(favicon_results_callback, tracker);
}

base::CancelableTaskTracker::TaskId FaviconService::GetFaviconForPageURL(
    const FaviconForPageURLParams& params,
    const favicon_base::FaviconResultsCallback& callback,
    base::CancelableTaskTracker* tracker) {
  return GetFaviconForPageURLImpl(
      params,
      GetPixelSizesForFaviconScales(params.desired_size_in_dip),
      callback,
      tracker);
}

base::CancelableTaskTracker::TaskId FaviconService::GetLargestRawFaviconForID(
    favicon_base::FaviconID favicon_id,
    const favicon_base::FaviconRawBitmapCallback& callback,
    base::CancelableTaskTracker* tracker) {
  // Use 0 as |desired_size| to get the largest bitmap for |favicon_id| without
  // any resizing.
  int desired_size = 0;
  favicon_base::FaviconResultsCallback callback_runner =
      Bind(&FaviconService::RunFaviconRawBitmapCallbackWithBitmapResults,
           base::Unretained(this),
           callback,
           desired_size);

  if (history_service_) {
    return history_service_->GetFaviconForID(
        favicon_id, desired_size, callback_runner, tracker);
  }
  return RunWithEmptyResultAsync(callback_runner, tracker);
}

void FaviconService::SetFaviconOutOfDateForPage(const GURL& page_url) {
  if (history_service_)
    history_service_->SetFaviconsOutOfDateForPage(page_url);
}

void FaviconService::CloneFavicon(const GURL& old_page_url,
                                  const GURL& new_page_url) {
  if (history_service_)
    history_service_->CloneFavicons(old_page_url, new_page_url);
}

void FaviconService::SetImportedFavicons(
    const std::vector<ImportedFaviconUsage>& favicon_usage) {
  if (history_service_)
    history_service_->SetImportedFavicons(favicon_usage);
}

void FaviconService::MergeFavicon(
    const GURL& page_url,
    const GURL& icon_url,
    favicon_base::IconType icon_type,
    scoped_refptr<base::RefCountedMemory> bitmap_data,
    const gfx::Size& pixel_size) {
  if (history_service_) {
    history_service_->MergeFavicon(page_url, icon_url, icon_type, bitmap_data,
                                   pixel_size);
  }
}

void FaviconService::SetFavicons(const GURL& page_url,
                                 const GURL& icon_url,
                                 favicon_base::IconType icon_type,
                                 const gfx::Image& image) {
  if (!history_service_)
    return;

  gfx::ImageSkia image_skia = image.AsImageSkia();
  image_skia.EnsureRepsForSupportedScales();
  const std::vector<gfx::ImageSkiaRep>& image_reps = image_skia.image_reps();
  std::vector<favicon_base::FaviconRawBitmapData> favicon_bitmap_data;
  for (size_t i = 0; i < image_reps.size(); ++i) {
    scoped_refptr<base::RefCountedBytes> bitmap_data(
        new base::RefCountedBytes());
    if (gfx::PNGCodec::EncodeBGRASkBitmap(image_reps[i].sk_bitmap(),
                                          false,
                                          &bitmap_data->data())) {
      gfx::Size pixel_size(image_reps[i].pixel_width(),
                           image_reps[i].pixel_height());
      favicon_base::FaviconRawBitmapData bitmap_data_element;
      bitmap_data_element.bitmap_data = bitmap_data;
      bitmap_data_element.pixel_size = pixel_size;
      bitmap_data_element.icon_url = icon_url;

      favicon_bitmap_data.push_back(bitmap_data_element);
    }
  }

  history_service_->SetFavicons(page_url, icon_type, favicon_bitmap_data);
}

void FaviconService::UnableToDownloadFavicon(const GURL& icon_url) {
  MissingFaviconURLHash url_hash = base::Hash(icon_url.spec());
  missing_favicon_urls_.insert(url_hash);
}

bool FaviconService::WasUnableToDownloadFavicon(const GURL& icon_url) const {
  MissingFaviconURLHash url_hash = base::Hash(icon_url.spec());
  return missing_favicon_urls_.find(url_hash) != missing_favicon_urls_.end();
}

void FaviconService::ClearUnableToDownloadFavicons() {
  missing_favicon_urls_.clear();
}

FaviconService::~FaviconService() {}

base::CancelableTaskTracker::TaskId FaviconService::GetFaviconForPageURLImpl(
    const FaviconForPageURLParams& params,
    const std::vector<int>& desired_sizes_in_pixel,
    const favicon_base::FaviconResultsCallback& callback,
    base::CancelableTaskTracker* tracker) {
  if (params.page_url.SchemeIs(content::kChromeUIScheme) ||
      params.page_url.SchemeIs(extensions::kExtensionScheme)) {
    return GetFaviconForChromeURL(
        profile_, params.page_url, desired_sizes_in_pixel, callback, tracker);
  }
  if (history_service_) {
    return history_service_->GetFaviconsForURL(params.page_url,
                                               params.icon_types,
                                               desired_sizes_in_pixel,
                                               callback,
                                               tracker);
  }
  return RunWithEmptyResultAsync(callback, tracker);
}

void FaviconService::RunFaviconImageCallbackWithBitmapResults(
    const favicon_base::FaviconImageCallback& callback,
    int desired_size_in_dip,
    const std::vector<favicon_base::FaviconRawBitmapResult>&
        favicon_bitmap_results) {
  favicon_base::FaviconImageResult image_result;
  image_result.image = favicon_base::SelectFaviconFramesFromPNGs(
      favicon_bitmap_results,
      favicon_base::GetFaviconScales(),
      desired_size_in_dip);
  favicon_base::SetFaviconColorSpace(&image_result.image);

  image_result.icon_url = image_result.image.IsEmpty() ?
      GURL() : favicon_bitmap_results[0].icon_url;
  callback.Run(image_result);
}

void FaviconService::RunFaviconRawBitmapCallbackWithBitmapResults(
    const favicon_base::FaviconRawBitmapCallback& callback,
    int desired_size_in_pixel,
    const std::vector<favicon_base::FaviconRawBitmapResult>&
        favicon_bitmap_results) {
  if (favicon_bitmap_results.empty() || !favicon_bitmap_results[0].is_valid()) {
    callback.Run(favicon_base::FaviconRawBitmapResult());
    return;
  }

  DCHECK_EQ(1u, favicon_bitmap_results.size());
  favicon_base::FaviconRawBitmapResult bitmap_result =
      favicon_bitmap_results[0];

  // If the desired size is 0, SelectFaviconFrames() will return the largest
  // bitmap without doing any resizing. As |favicon_bitmap_results| has bitmap
  // data for a single bitmap, return it and avoid an unnecessary decode.
  if (desired_size_in_pixel == 0) {
    callback.Run(bitmap_result);
    return;
  }

  // If history bitmap is already desired pixel size, return early.
  if (bitmap_result.pixel_size.width() == desired_size_in_pixel &&
      bitmap_result.pixel_size.height() == desired_size_in_pixel) {
    callback.Run(bitmap_result);
    return;
  }

  // Convert raw bytes to SkBitmap, resize via SelectFaviconFrames(), then
  // convert back.
  std::vector<float> desired_favicon_scales;
  desired_favicon_scales.push_back(1.0f);
  gfx::Image resized_image = favicon_base::SelectFaviconFramesFromPNGs(
      favicon_bitmap_results, desired_favicon_scales, desired_size_in_pixel);

  std::vector<unsigned char> resized_bitmap_data;
  if (!gfx::PNGCodec::EncodeBGRASkBitmap(resized_image.AsBitmap(), false,
                                         &resized_bitmap_data)) {
    callback.Run(favicon_base::FaviconRawBitmapResult());
    return;
  }

  bitmap_result.bitmap_data = base::RefCountedBytes::TakeVector(
      &resized_bitmap_data);
  callback.Run(bitmap_result);
}
