// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/drive/change_list_processor.h"

#include <utility>

#include "base/metrics/histogram.h"
#include "chrome/browser/chromeos/drive/drive.pb.h"
#include "chrome/browser/chromeos/drive/drive_resource_metadata.h"
#include "chrome/browser/chromeos/drive/resource_entry_conversion.h"
#include "chrome/browser/google_apis/gdata_wapi_parser.h"
#include "content/public/browser/browser_thread.h"

using content::BrowserThread;

namespace drive {

namespace {

// Callback for DriveResourceMetadata::SetLargestChangestamp.
// Runs |on_complete_callback|. |on_complete_callback| must not be null.
void RunOnCompleteCallback(const base::Closure& on_complete_callback,
                           DriveFileError error) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!on_complete_callback.is_null());
  DCHECK_EQ(DRIVE_FILE_OK, error);

  on_complete_callback.Run();
}

}  // namespace

class ChangeListProcessor::ChangeListToEntryProtoMapUMAStats {
 public:
  ChangeListToEntryProtoMapUMAStats()
    : num_regular_files_(0),
      num_hosted_documents_(0) {
  }

  // Increment number of files.
  void IncrementNumFiles(bool is_hosted_document) {
    is_hosted_document ? num_hosted_documents_++ : num_regular_files_++;
  }

  // Updates UMA histograms with file counts.
  void UpdateFileCountUmaHistograms() {
    const int num_total_files = num_hosted_documents_ + num_regular_files_;
    UMA_HISTOGRAM_COUNTS("Drive.NumberOfRegularFiles", num_regular_files_);
    UMA_HISTOGRAM_COUNTS("Drive.NumberOfHostedDocuments",
                         num_hosted_documents_);
    UMA_HISTOGRAM_COUNTS("Drive.NumberOfTotalFiles", num_total_files);
  }

 private:
  int num_regular_files_;
  int num_hosted_documents_;
};

ChangeListProcessor::ChangeListProcessor(
    DriveResourceMetadata* resource_metadata)
  : resource_metadata_(resource_metadata),
    largest_changestamp_(0),
    ALLOW_THIS_IN_INITIALIZER_LIST(weak_ptr_factory_(this)) {
}

ChangeListProcessor::~ChangeListProcessor() {
}

void ChangeListProcessor::ApplyFeeds(
    const ScopedVector<google_apis::ResourceList>& feed_list,
    bool is_delta_feed,
    int64 root_feed_changestamp,
    const base::Closure& on_complete_callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!on_complete_callback.is_null());

  int64 delta_feed_changestamp = 0;
  ChangeListToEntryProtoMapUMAStats uma_stats;
  FeedToEntryProtoMap(feed_list, &delta_feed_changestamp, &uma_stats);
  // Note FeedToEntryProtoMap calls Clear() which resets on_complete_callback_.
  on_complete_callback_ = on_complete_callback;
  largest_changestamp_ =
      is_delta_feed ? delta_feed_changestamp : root_feed_changestamp;
  ApplyEntryProtoMap(is_delta_feed);

  // Shouldn't record histograms when processing delta feeds.
  if (!is_delta_feed)
    uma_stats.UpdateFileCountUmaHistograms();
}

void ChangeListProcessor::ApplyEntryProtoMap(bool is_delta_feed) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  if (!is_delta_feed) {  // Full update.
    changed_dirs_.insert(base::FilePath(kDriveRootDirectory));
    resource_metadata_->RemoveAll(
        base::Bind(&ChangeListProcessor::ApplyNextEntryProtoAsync,
                   weak_ptr_factory_.GetWeakPtr()));
  } else {
    // Go through all entries generated by the feed and apply them to the local
    // snapshot of the file system.
    ApplyNextEntryProtoAsync();
  }
}

void ChangeListProcessor::ApplyNextEntryProtoAsync() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  base::MessageLoopProxy::current()->PostTask(
      FROM_HERE,
      base::Bind(&ChangeListProcessor::ApplyNextEntryProto,
                 weak_ptr_factory_.GetWeakPtr()));
}

void ChangeListProcessor::ApplyNextEntryProto() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  if (!entry_proto_map_.empty())
    ApplyNextByIterator(entry_proto_map_.begin());  // Continue.
  else if (root_upload_url_.is_valid())
    UpdateRootUploadUrl();  // Set root_upload_url_ before we finish.
  else
    OnComplete();  // Finished.
}

void ChangeListProcessor::ApplyNextByIterator(DriveEntryProtoMap::iterator it) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  DriveEntryProto entry_proto = it->second;
  DCHECK_EQ(it->first, entry_proto.resource_id());

  // The parent of this entry may not yet be processed. We need the parent
  // to be rooted in the metadata tree before we can add the child, so process
  // the parent first.
  DriveEntryProtoMap::iterator parent_it = entry_proto_map_.find(
      entry_proto.parent_resource_id());
  if (parent_it != entry_proto_map_.end()) {
    base::MessageLoopProxy::current()->PostTask(
        FROM_HERE,
        base::Bind(&ChangeListProcessor::ApplyNextByIterator,
                   weak_ptr_factory_.GetWeakPtr(),
                   parent_it));
  } else {
    // Erase the entry so the deleted entry won't be referenced.
    entry_proto_map_.erase(it);
    ApplyEntryProto(entry_proto);
  }
}

void ChangeListProcessor::ApplyEntryProto(const DriveEntryProto& entry_proto) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  // Lookup the entry.
  resource_metadata_->GetEntryInfoByResourceId(
      entry_proto.resource_id(),
      base::Bind(&ChangeListProcessor::ContinueApplyEntryProto,
                 weak_ptr_factory_.GetWeakPtr(),
                 entry_proto));
}

void ChangeListProcessor::ContinueApplyEntryProto(
    const DriveEntryProto& entry_proto,
    DriveFileError error,
    const base::FilePath& file_path,
    scoped_ptr<DriveEntryProto> old_entry_proto) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  if (error == DRIVE_FILE_OK) {
    if (entry_proto.deleted()) {
      // Deleted file/directory.
      RemoveEntryFromParent(entry_proto, file_path);
    } else {
      // Entry exists and needs to be refreshed.
      RefreshEntry(entry_proto, file_path);
    }
  } else if (error == DRIVE_FILE_ERROR_NOT_FOUND && !entry_proto.deleted()) {
    // Adding a new entry.
    AddEntry(entry_proto);
  } else {
    // Continue.
    ApplyNextEntryProtoAsync();
  }
}

void ChangeListProcessor::AddEntry(const DriveEntryProto& entry_proto) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  resource_metadata_->AddEntry(
      entry_proto,
      base::Bind(&ChangeListProcessor::NotifyForAddEntry,
                 weak_ptr_factory_.GetWeakPtr(),
                 entry_proto.file_info().is_directory()));
}

void ChangeListProcessor::NotifyForAddEntry(bool is_directory,
                                            DriveFileError error,
                                            const base::FilePath& file_path) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  DVLOG(1) << "NotifyForAddEntry " << file_path.value();
  if (error == DRIVE_FILE_OK) {
    // Notify if a directory has been created.
    if (is_directory)
      changed_dirs_.insert(file_path);

    // Notify parent.
    changed_dirs_.insert(file_path.DirName());
  }

  ApplyNextEntryProtoAsync();
}

void ChangeListProcessor::RemoveEntryFromParent(
    const DriveEntryProto& entry_proto,
    const base::FilePath& file_path) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!file_path.empty());

  if (!entry_proto.file_info().is_directory()) {
    // No children if entry is a file.
    OnGetChildrenForRemove(entry_proto, file_path, std::set<base::FilePath>());
  } else {
    // If entry is a directory, notify its children.
    resource_metadata_->GetChildDirectories(
        entry_proto.resource_id(),
        base::Bind(&ChangeListProcessor::OnGetChildrenForRemove,
                   weak_ptr_factory_.GetWeakPtr(),
                   entry_proto,
                   file_path));
  }
}

void ChangeListProcessor::OnGetChildrenForRemove(
    const DriveEntryProto& entry_proto,
    const base::FilePath& file_path,
    const std::set<base::FilePath>& child_directories) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!file_path.empty());

  resource_metadata_->RemoveEntryFromParent(
      entry_proto.resource_id(),
      base::Bind(&ChangeListProcessor::NotifyForRemoveEntryFromParent,
                 weak_ptr_factory_.GetWeakPtr(),
                 entry_proto.file_info().is_directory(),
                 file_path,
                 child_directories));
}

void ChangeListProcessor::NotifyForRemoveEntryFromParent(
    bool is_directory,
    const base::FilePath& file_path,
    const std::set<base::FilePath>& child_directories,
    DriveFileError error,
    const base::FilePath& parent_path) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  DVLOG(1) << "NotifyForRemoveEntryFromParent " << file_path.value();
  if (error == DRIVE_FILE_OK) {
    // Notify parent.
    changed_dirs_.insert(parent_path);

    // Notify children, if any.
    changed_dirs_.insert(child_directories.begin(),
                         child_directories.end());

    // If entry is a directory, notify self.
    if (is_directory)
      changed_dirs_.insert(file_path);
  }

  // Continue.
  ApplyNextEntryProtoAsync();
}

void ChangeListProcessor::RefreshEntry(const DriveEntryProto& entry_proto,
                                      const base::FilePath& file_path) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  resource_metadata_->RefreshEntry(
      entry_proto,
      base::Bind(&ChangeListProcessor::NotifyForRefreshEntry,
                 weak_ptr_factory_.GetWeakPtr(),
                 file_path));
}

void ChangeListProcessor::NotifyForRefreshEntry(
    const base::FilePath& old_file_path,
    DriveFileError error,
    const base::FilePath& file_path,
    scoped_ptr<DriveEntryProto> entry_proto) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  DVLOG(1) << "NotifyForRefreshEntry " << file_path.value();
  if (error == DRIVE_FILE_OK) {
    // Notify old parent.
    changed_dirs_.insert(old_file_path.DirName());

    // Notify new parent.
    changed_dirs_.insert(file_path.DirName());

    // Notify self if entry is a directory.
    if (entry_proto->file_info().is_directory()) {
      // Notify new self.
      changed_dirs_.insert(file_path);
      // Notify old self.
      changed_dirs_.insert(old_file_path);
    }
  }

  ApplyNextEntryProtoAsync();
}

void ChangeListProcessor::FeedToEntryProtoMap(
    const ScopedVector<google_apis::ResourceList>& feed_list,
    int64* feed_changestamp,
    ChangeListToEntryProtoMapUMAStats* uma_stats) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  Clear();

  for (size_t i = 0; i < feed_list.size(); ++i) {
    const google_apis::ResourceList* feed = feed_list[i];

    // Get upload url from the root feed. Links for all other collections will
    // be handled in ConvertResourceEntryToDriveEntryProto.
    if (i == 0) {
      const google_apis::Link* root_feed_upload_link = feed->GetLinkByType(
          google_apis::Link::LINK_RESUMABLE_CREATE_MEDIA);
      if (root_feed_upload_link)
        root_upload_url_ = root_feed_upload_link->href();
      if (feed_changestamp)
        *feed_changestamp = feed->largest_changestamp();
      DCHECK_GE(feed->largest_changestamp(), 0);
    }

    for (size_t j = 0; j < feed->entries().size(); ++j) {
      const google_apis::ResourceEntry* entry = feed->entries()[j];
      DriveEntryProto entry_proto =
          ConvertResourceEntryToDriveEntryProto(*entry);
      // Some document entries don't map into files (i.e. sites).
      if (entry_proto.resource_id().empty())
        continue;

      // Count the number of files.
      if (uma_stats && !entry_proto.file_info().is_directory()) {
        uma_stats->IncrementNumFiles(
            entry_proto.file_specific_info().is_hosted_document());
      }

      std::pair<DriveEntryProtoMap::iterator, bool> ret = entry_proto_map_.
          insert(std::make_pair(entry_proto.resource_id(), entry_proto));
      DCHECK(ret.second);
      if (!ret.second)
        LOG(WARNING) << "Found duplicate file " << entry_proto.base_name();
    }
  }
}

void ChangeListProcessor::UpdateRootUploadUrl() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(root_upload_url_.is_valid());

  resource_metadata_->GetEntryInfoByPath(
      base::FilePath(kDriveRootDirectory),
      base::Bind(&ChangeListProcessor::OnGetRootEntryProto,
                 weak_ptr_factory_.GetWeakPtr()));
}

void ChangeListProcessor::OnGetRootEntryProto(
    DriveFileError error,
    scoped_ptr<DriveEntryProto> root_proto) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  if (error != DRIVE_FILE_OK) {
    // TODO(satorux): Need to trigger recovery if root is corrupt.
    LOG(WARNING) << "Failed to get the proto for root directory";
    OnComplete();
    return;
  }
  DCHECK(root_proto.get());
  root_proto->set_upload_url(root_upload_url_.spec());
  resource_metadata_->RefreshEntry(
      *root_proto,
      base::Bind(&ChangeListProcessor::OnUpdateRootUploadUrl,
                 weak_ptr_factory_.GetWeakPtr()));
}

void ChangeListProcessor::OnUpdateRootUploadUrl(
    DriveFileError error,
    const base::FilePath& /* root_path */,
    scoped_ptr<DriveEntryProto> /* root_proto */) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  LOG_IF(WARNING, error != DRIVE_FILE_OK) << "Failed to refresh root directory";

  OnComplete();
}

void ChangeListProcessor::OnComplete() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  resource_metadata_->set_loaded(true);
  resource_metadata_->SetLargestChangestamp(
      largest_changestamp_,
      base::Bind(&RunOnCompleteCallback, on_complete_callback_));
}

void ChangeListProcessor::Clear() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  entry_proto_map_.clear();
  changed_dirs_.clear();
  root_upload_url_ = GURL();
  largest_changestamp_ = 0;
  on_complete_callback_.Reset();
}

}  // namespace drive
