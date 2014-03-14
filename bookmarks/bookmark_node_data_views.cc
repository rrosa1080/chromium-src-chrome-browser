// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/bookmarks/bookmark_node_data.h"

#include "base/logging.h"
#include "base/pickle.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/common/url_constants.h"
#include "ui/base/clipboard/clipboard.h"

void BookmarkNodeData::Write(Profile* profile, ui::OSExchangeData* data) const {
  DCHECK(data);

  // If there is only one element and it is a URL, write the URL to the
  // clipboard.
  if (elements.size() == 1 && elements[0].is_url) {
    if (elements[0].url.SchemeIs(content::kJavaScriptScheme)) {
      data->SetString(base::UTF8ToUTF16(elements[0].url.spec()));
    } else {
      data->SetURL(elements[0].url, elements[0].title);
    }
  }

  Pickle data_pickle;
  WriteToPickle(profile, &data_pickle);

  data->SetPickledData(GetFormatType(), data_pickle);
}

bool BookmarkNodeData::Read(const ui::OSExchangeData& data) {
  elements.clear();

  profile_path_.clear();

  if (data.HasCustomFormat(GetFormatType())) {
    Pickle drag_data_pickle;
    if (data.GetPickledData(GetFormatType(), &drag_data_pickle)) {
      if (!ReadFromPickle(&drag_data_pickle))
        return false;
    }
  } else {
    // See if there is a URL on the clipboard.
    Element element;
    GURL url;
    base::string16 title;
    if (data.GetURLAndTitle(
            ui::OSExchangeData::CONVERT_FILENAMES, &url, &title))
      ReadFromTuple(url, title);
  }

  return is_valid();
}
