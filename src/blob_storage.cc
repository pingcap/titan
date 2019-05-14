#include "blob_storage.h"
#include "version_set.h"

namespace rocksdb {
namespace titandb {

Status BlobStorage::Get(const ReadOptions& options, const BlobIndex& index,
                        BlobRecord* record, PinnableSlice* buffer) {
  auto sfile = FindFile(index.file_number).lock();
  if (!sfile)
    return Status::Corruption("Missing blob file: " +
                              std::to_string(index.file_number));
  return file_cache_->Get(options, sfile->file_number(), sfile->file_size(),
                          index.blob_handle, record, buffer);
}

Status BlobStorage::NewPrefetcher(uint64_t file_number,
                                  std::unique_ptr<BlobFilePrefetcher>* result) {
  auto sfile = FindFile(file_number).lock();
  if (!sfile)
    return Status::Corruption("Missing blob wfile: " +
                              std::to_string(file_number));
  return file_cache_->NewPrefetcher(sfile->file_number(), sfile->file_size(),
                                    result);
}

std::weak_ptr<BlobFileMeta> BlobStorage::FindFile(uint64_t file_number) const {
  ReadLock rl(&mutex_);
  auto it = files_.find(file_number);
  if (it != files_.end()) {
    assert(file_number == it->second->file_number());
    return it->second;
  }
  return std::weak_ptr<BlobFileMeta>();
}

void BlobStorage::ExportBlobFiles(
    std::map<uint64_t, std::weak_ptr<BlobFileMeta>>& ret) const {
  ReadLock rl(&mutex_);
  for (auto& kv : files_) {
    ret.emplace(kv.first, std::weak_ptr<BlobFileMeta>(kv.second));
  }
}

void BlobStorage::AddBlobFile(std::shared_ptr<BlobFileMeta>& file) {
  WriteLock wl(&mutex_);
  files_.emplace(std::make_pair(file->file_number(), file));
}

void BlobStorage::MarkFileObsolete(std::shared_ptr<BlobFileMeta> file,
                                   SequenceNumber obsolete_sequence) {
  WriteLock wl(&mutex_);
  obsolete_files_.push_back(
      std::make_pair(file->file_number(), obsolete_sequence));
  file->FileStateTransit(BlobFileMeta::FileEvent::kDelete);
}

void BlobStorage::GetObsoleteFiles(std::vector<std::string>* obsolete_files,
                                   SequenceNumber oldest_sequence) {
  WriteLock wl(&mutex_);

  for (auto it = obsolete_files_.begin(); it != obsolete_files_.end();) {
    auto& file_number = it->first;
    auto& obsolete_sequence = it->second;
    // We check whether the oldest snapshot is no less than the last sequence
    // by the time the blob file become obsolete. If so, the blob file is not
    // visible to all existing snapshots.
    if (oldest_sequence > obsolete_sequence) {
      // remove obsolete files
      files_.erase(file_number);
      file_cache_->Evict(file_number);

      ROCKS_LOG_INFO(db_options_.info_log,
                     "Obsolete blob file %" PRIu64 " (obsolete at %" PRIu64
                     ") not visible to oldest snapshot %" PRIu64 ", delete it.",
                     file_number, obsolete_sequence, oldest_sequence);
      if (obsolete_files) {
        obsolete_files->emplace_back(
            BlobFileName(db_options_.dirname, file_number));
      }

      it = obsolete_files_.erase(it);
      continue;
    }
    ++it;
  }
}

void BlobStorage::ComputeGCScore() {
  // TODO: no need to recompute all everytime
  gc_score_.clear();

  {
    ReadLock rl(&mutex_);
    for (auto& file : files_) {
      if (file.second->is_obsolete()) {
        continue;
      }
      gc_score_.push_back({});
      auto& gcs = gc_score_.back();
      gcs.file_number = file.first;
      if (file.second->file_size() < cf_options_.merge_small_file_threshold) {
        gcs.score = 1;
      } else {
        gcs.score = file.second->GetDiscardableRatio();
      }
    }
  }

  std::sort(gc_score_.begin(), gc_score_.end(),
            [](const GCScore& first, const GCScore& second) {
              return first.score > second.score;
            });
}

}  // namespace titandb
}  // namespace rocksdb
