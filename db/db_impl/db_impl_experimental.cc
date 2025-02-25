//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <cinttypes>
#include <vector>

#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/job_context.h"
#include "db/version_set.h"
#include "rocksdb/status.h"
#include "util/cast_util.h"

namespace ROCKSDB_NAMESPACE {

#ifndef ROCKSDB_LITE
Status DBImpl::SuggestCompactRange(ColumnFamilyHandle* column_family,
                                   const Slice* begin, const Slice* end) {
  auto cfh = static_cast_with_check<ColumnFamilyHandleImpl>(column_family);
  auto cfd = cfh->cfd();
  InternalKey start_key, end_key;
  if (begin != nullptr) {
    start_key.SetMinPossibleForUserKey(*begin);
  }
  if (end != nullptr) {
    end_key.SetMaxPossibleForUserKey(*end);
  }
  {
    InstrumentedMutexLock l(&mutex_);
    auto vstorage = cfd->current()->storage_info();
    for (int level = 0; level < vstorage->num_non_empty_levels() - 1; ++level) {
      std::vector<FileMetaData*> inputs;
      vstorage->GetOverlappingInputs(
          level, begin == nullptr ? nullptr : &start_key,
          end == nullptr ? nullptr : &end_key, &inputs);
      for (auto f : inputs) {
        f->marked_for_compaction = true;
      }
    }
    // Since we have some more files to compact, we should also recompute
    // compaction score
    vstorage->ComputeCompactionScore(*cfd->ioptions(),
                                     *cfd->GetLatestMutableCFOptions());
    SchedulePendingCompaction(cfd);
    MaybeScheduleFlushOrCompaction();
  }
  return Status::OK();
}

Status DBImpl::PromoteL0(ColumnFamilyHandle* column_family, int target_level) {
  assert(column_family);

  if (target_level < 1) {
    ROCKS_LOG_INFO(immutable_db_options_.info_log,
                   "PromoteL0 FAILED. Invalid target level %d\n", target_level);
    return Status::InvalidArgument("Invalid target level");
  }

  Status status;
  VersionEdit edit;
  JobContext job_context(next_job_id_.fetch_add(1), true);
  {
    InstrumentedMutexLock l(&mutex_);
    auto* cfd = static_cast<ColumnFamilyHandleImpl*>(column_family)->cfd();
    const auto* vstorage = cfd->current()->storage_info();

    if (target_level >= vstorage->num_levels()) {
      ROCKS_LOG_INFO(immutable_db_options_.info_log,
                     "PromoteL0 FAILED. Target level %d does not exist\n",
                     target_level);
      job_context.Clean();
      return Status::InvalidArgument("Target level does not exist");
    }

    // Sort L0 files by range.
    const InternalKeyComparator* icmp = &cfd->internal_comparator();
    auto l0_files = vstorage->LevelFiles(0);
    std::sort(l0_files.begin(), l0_files.end(),
              [icmp](FileMetaData* f1, FileMetaData* f2) {
                return icmp->Compare(f1->largest, f2->largest) < 0;
              });

    // Check that no L0 file is being compacted and that they have
    // non-overlapping ranges.
    for (size_t i = 0; i < l0_files.size(); ++i) {
      auto f = l0_files[i];
      if (f->being_compacted) {
        ROCKS_LOG_INFO(immutable_db_options_.info_log,
                       "PromoteL0 FAILED. File %" PRIu64 " being compacted\n",
                       f->fd.GetNumber());
        job_context.Clean();
        return Status::InvalidArgument("PromoteL0 called during L0 compaction");
      }

      if (i == 0) continue;
      auto prev_f = l0_files[i - 1];
      if (icmp->Compare(prev_f->largest, f->smallest) >= 0) {
        ROCKS_LOG_INFO(immutable_db_options_.info_log,
                       "PromoteL0 FAILED. Files %" PRIu64 " and %" PRIu64
                       " have overlapping ranges\n",
                       prev_f->fd.GetNumber(), f->fd.GetNumber());
        job_context.Clean();
        return Status::InvalidArgument("L0 has overlapping files");
      }
    }

    // Check that all levels up to target_level are empty.
    for (int level = 1; level <= target_level; ++level) {
      if (vstorage->NumLevelFiles(level) > 0) {
        ROCKS_LOG_INFO(immutable_db_options_.info_log,
                       "PromoteL0 FAILED. Level %d not empty\n", level);
        job_context.Clean();
        return Status::InvalidArgument(
            "All levels up to target_level "
            "must be empty");
      }
    }

    edit.SetColumnFamily(cfd->GetID());
    for (const auto& f : l0_files) {
      edit.DeleteFile(0, f->fd.GetNumber());
      edit.AddFile(target_level, f->fd.GetNumber(), f->fd.GetPathId(),
                   f->fd.GetFileSize(), f->smallest, f->largest,
                   f->fd.smallest_seqno, f->fd.largest_seqno,
                   f->marked_for_compaction, f->oldest_blob_file_number,
                   f->oldest_ancester_time, f->file_creation_time,
                   f->file_checksum, f->file_checksum_func_name);
    }

    status = versions_->LogAndApply(cfd, *cfd->GetLatestMutableCFOptions(),
                                    &edit, &mutex_, directories_.GetDbDir());
    if (status.ok()) {
      InstallSuperVersionAndScheduleWork(cfd,
                                         &job_context.superversion_contexts[0],
                                         *cfd->GetLatestMutableCFOptions());
    }
  }  // lock released here
  LogFlush(immutable_db_options_.info_log);
  job_context.Clean();

  return status;
}

Status DBImpl::PromoteLastL0File(ColumnFamilyHandle* column_family, int target_level) {
  assert(column_family);

  if (target_level < 1) {
    ROCKS_LOG_INFO(immutable_db_options_.info_log,
                   "PromoteLastL0File FAILED. Invalid target level %d\n",
                   target_level);
    return Status::InvalidArgument("Invalid target level");
  }

  Status status;
  VersionEdit edit;
  JobContext job_context(next_job_id_.fetch_add(1), true);
  {
    InstrumentedMutexLock l(&mutex_);
    auto* cfd = static_cast<ColumnFamilyHandleImpl*>(column_family)->cfd();
    const auto* vstorage = cfd->current()->storage_info();

    if (target_level >= vstorage->num_levels()) {
      ROCKS_LOG_INFO(immutable_db_options_.info_log,
                     "PromoteLastL0File FAILED. Target level %d does"
                     " not exist\n",
                     target_level);
      job_context.Clean();
      return Status::InvalidArgument("Target level does not exist");
    }

    // Sort L0 files by range.
    auto l0_files = vstorage->LevelFiles(0);

    if(l0_files.empty()) {
      ROCKS_LOG_INFO(immutable_db_options_.info_log,
                     "PromoteLastL0File FAILED. Level 0 is empty");
      job_context.Clean();
      return Status::InvalidArgument("Level 0 is empty");
    }

    auto f = l0_files.back();

    // Check that no L0 file is being compacted and that they have
    // non-overlapping ranges.
    if (f->being_compacted) {
      ROCKS_LOG_INFO(immutable_db_options_.info_log,
                      "PromoteLastL0File FAILED. File %" PRIu64 
                      " being compacted\n",
                      f->fd.GetNumber());
      job_context.Clean();
      return Status::InvalidArgument("PromoteLastL0File called during"
                                     " L0 compaction");
    }

    // Check that all levels up to target_level are empty.
    for (int level = 1; level <= target_level; ++level) {
      if (vstorage->NumLevelFiles(level) > 0) {
        ROCKS_LOG_INFO(immutable_db_options_.info_log,
                       "PromoteLastL0File FAILED. Level %d not empty\n", level);
        job_context.Clean();
        return Status::InvalidArgument(
            "All levels up to target_level "
            "must be empty");
      }
    }

    edit.SetColumnFamily(cfd->GetID());
    edit.DeleteFile(0, f->fd.GetNumber());
    edit.AddFile(target_level, f->fd.GetNumber(), f->fd.GetPathId(),
                  f->fd.GetFileSize(), f->smallest, f->largest,
                  f->fd.smallest_seqno, f->fd.largest_seqno,
                  f->marked_for_compaction, f->oldest_blob_file_number,
                  f->oldest_ancester_time, f->file_creation_time,
                  f->file_checksum, f->file_checksum_func_name);

    status = versions_->LogAndApply(cfd, *cfd->GetLatestMutableCFOptions(),
                                    &edit, &mutex_, directories_.GetDbDir());
    if (status.ok()) {
      InstallSuperVersionAndScheduleWork(cfd,
                                         &job_context.superversion_contexts[0],
                                         *cfd->GetLatestMutableCFOptions());
    }
  }  // lock released here
  LogFlush(immutable_db_options_.info_log);
  job_context.Clean();

  return status;
}

Status DBImpl::SuggestPartialL0Compaction(ColumnFamilyHandle* column_family,
                                          int files_to_keep) {
  auto cfh = static_cast_with_check<ColumnFamilyHandleImpl>(column_family);
  auto cfd = cfh->cfd();
  {
    InstrumentedMutexLock l(&mutex_);
    auto vstorage = cfd->current()->storage_info();
    auto l0_files = vstorage->LevelFiles(0);

    for (size_t i = files_to_keep; i < l0_files.size(); ++i) {
      auto& f = l0_files[i];
      if (f->being_compacted) {
        continue;
      }
      f->marked_for_compaction = true;
    }

    // Since we have some more files to compact, we should also recompute
    // compaction score
    vstorage->ComputeCompactionScore(*cfd->ioptions(),
                                     *cfd->GetLatestMutableCFOptions());
    SchedulePendingCompaction(cfd);
    MaybeScheduleFlushOrCompaction();
  }
  return Status::OK();
}

Status DBImpl::SuggestLevelCompaction(ColumnFamilyHandle* column_family,
                                      int level) {
  if (level < 1) {
    ROCKS_LOG_INFO(immutable_db_options_.info_log,
                   "SuggestLevelCompaction FAILED. Invalid level %d\n",
                   level);
    return Status::InvalidArgument("Invalid level");
  }

  auto cfh = static_cast_with_check<ColumnFamilyHandleImpl>(column_family);
  auto cfd = cfh->cfd();
  {
    InstrumentedMutexLock l(&mutex_);
    auto vstorage = cfd->current()->storage_info();

    if (level >= vstorage->num_levels()) {
      ROCKS_LOG_INFO(immutable_db_options_.info_log,
                     "SuggestLevelCompaction FAILED. Level %d does"
                     " not exist\n",
                     level);
      return Status::InvalidArgument("Level does not exist");
    }

    for (auto& f : vstorage->LevelFiles(level)) {
      if (f->being_compacted) {
        continue;
      }
      f->marked_for_compaction = true;
    }

    // Since we have some more files to compact, we should also recompute
    // compaction score
    vstorage->ComputeCompactionScore(*cfd->ioptions(),
                                     *cfd->GetLatestMutableCFOptions());
    SchedulePendingCompaction(cfd);
    MaybeScheduleFlushOrCompaction();
  }
  return Status::OK();
}

#endif  // ROCKSDB_LITE

}  // namespace ROCKSDB_NAMESPACE
