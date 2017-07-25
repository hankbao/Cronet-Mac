// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/prefs/json_pref_store.h"

#include <stddef.h>

#include <algorithm>
#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_file_value_serializer.h"
#include "base/json/json_string_value_serializer.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/metrics/histogram.h"
#include "base/sequenced_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task_runner_util.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/threading/sequenced_worker_pool.h"
#include "base/time/default_clock.h"
#include "base/values.h"
#include "components/prefs/pref_filter.h"

// Result returned from internal read tasks.
struct JsonPrefStore::ReadResult {
 public:
  ReadResult();
  ~ReadResult();

  std::unique_ptr<base::Value> value;
  PrefReadError error;
  bool no_dir;

 private:
  DISALLOW_COPY_AND_ASSIGN(ReadResult);
};

JsonPrefStore::ReadResult::ReadResult()
    : error(PersistentPrefStore::PREF_READ_ERROR_NONE), no_dir(false) {
}

JsonPrefStore::ReadResult::~ReadResult() {
}

namespace {

// Some extensions we'll tack on to copies of the Preferences files.
const base::FilePath::CharType kBadExtension[] = FILE_PATH_LITERAL("bad");

PersistentPrefStore::PrefReadError HandleReadErrors(
    const base::Value* value,
    const base::FilePath& path,
    int error_code,
    const std::string& error_msg) {
  if (!value) {
    DVLOG(1) << "Error while loading JSON file: " << error_msg
             << ", file: " << path.value();
    switch (error_code) {
      case JSONFileValueDeserializer::JSON_ACCESS_DENIED:
        return PersistentPrefStore::PREF_READ_ERROR_ACCESS_DENIED;
      case JSONFileValueDeserializer::JSON_CANNOT_READ_FILE:
        return PersistentPrefStore::PREF_READ_ERROR_FILE_OTHER;
      case JSONFileValueDeserializer::JSON_FILE_LOCKED:
        return PersistentPrefStore::PREF_READ_ERROR_FILE_LOCKED;
      case JSONFileValueDeserializer::JSON_NO_SUCH_FILE:
        return PersistentPrefStore::PREF_READ_ERROR_NO_FILE;
      default:
        // JSON errors indicate file corruption of some sort.
        // Since the file is corrupt, move it to the side and continue with
        // empty preferences.  This will result in them losing their settings.
        // We keep the old file for possible support and debugging assistance
        // as well as to detect if they're seeing these errors repeatedly.
        // TODO(erikkay) Instead, use the last known good file.
        base::FilePath bad = path.ReplaceExtension(kBadExtension);

        // If they've ever had a parse error before, put them in another bucket.
        // TODO(erikkay) if we keep this error checking for very long, we may
        // want to differentiate between recent and long ago errors.
        bool bad_existed = base::PathExists(bad);
        base::Move(path, bad);
        return bad_existed ? PersistentPrefStore::PREF_READ_ERROR_JSON_REPEAT
                           : PersistentPrefStore::PREF_READ_ERROR_JSON_PARSE;
    }
  }
  if (!value->IsType(base::Value::Type::DICTIONARY))
    return PersistentPrefStore::PREF_READ_ERROR_JSON_TYPE;
  return PersistentPrefStore::PREF_READ_ERROR_NONE;
}

// Records a sample for |size| in the Settings.JsonDataReadSizeKilobytes
// histogram suffixed with the base name of the JSON file under |path|.
void RecordJsonDataSizeHistogram(const base::FilePath& path, size_t size) {
  std::string spaceless_basename;
  base::ReplaceChars(path.BaseName().MaybeAsASCII(), " ", "_",
                     &spaceless_basename);

  // The histogram below is an expansion of the UMA_HISTOGRAM_CUSTOM_COUNTS
  // macro adapted to allow for a dynamically suffixed histogram name.
  // Note: The factory creates and owns the histogram.
  base::HistogramBase* histogram = base::Histogram::FactoryGet(
      "Settings.JsonDataReadSizeKilobytes." + spaceless_basename, 1, 10000, 50,
      base::HistogramBase::kUmaTargetedHistogramFlag);
  histogram->Add(static_cast<int>(size) / 1024);
}

std::unique_ptr<JsonPrefStore::ReadResult> ReadPrefsFromDisk(
    const base::FilePath& path) {
  int error_code;
  std::string error_msg;
  std::unique_ptr<JsonPrefStore::ReadResult> read_result(
      new JsonPrefStore::ReadResult);
  JSONFileValueDeserializer deserializer(path);
  read_result->value = deserializer.Deserialize(&error_code, &error_msg);
  read_result->error =
      HandleReadErrors(read_result->value.get(), path, error_code, error_msg);
  read_result->no_dir = !base::PathExists(path.DirName());

  if (read_result->error == PersistentPrefStore::PREF_READ_ERROR_NONE)
    RecordJsonDataSizeHistogram(path, deserializer.get_last_read_size());

  return read_result;
}

}  // namespace

// static
scoped_refptr<base::SequencedTaskRunner> JsonPrefStore::GetTaskRunnerForFile(
    const base::FilePath& filename,
    base::SequencedWorkerPool* worker_pool) {
  std::string token("json_pref_store-");
  token.append(filename.AsUTF8Unsafe());
  return worker_pool->GetSequencedTaskRunnerWithShutdownBehavior(
      worker_pool->GetNamedSequenceToken(token),
      base::SequencedWorkerPool::BLOCK_SHUTDOWN);
}

JsonPrefStore::JsonPrefStore(
    const base::FilePath& pref_filename,
    scoped_refptr<base::SequencedTaskRunner> file_task_runner,
    std::unique_ptr<PrefFilter> pref_filter)
    : path_(pref_filename),
      file_task_runner_(std::move(file_task_runner)),
      prefs_(new base::DictionaryValue()),
      read_only_(false),
      writer_(pref_filename, file_task_runner_),
      pref_filter_(std::move(pref_filter)),
      initialized_(false),
      filtering_in_progress_(false),
      pending_lossy_write_(false),
      read_error_(PREF_READ_ERROR_NONE),
      has_pending_write_reply_(false),
      write_count_histogram_(writer_.commit_interval(), path_) {
  DCHECK(!path_.empty());
}

bool JsonPrefStore::GetValue(const std::string& key,
                             const base::Value** result) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  base::Value* tmp = nullptr;
  if (!prefs_->Get(key, &tmp))
    return false;

  if (result)
    *result = tmp;
  return true;
}

std::unique_ptr<base::DictionaryValue> JsonPrefStore::GetValues() const {
  return prefs_->CreateDeepCopy();
}

void JsonPrefStore::AddObserver(PrefStore::Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  observers_.AddObserver(observer);
}

void JsonPrefStore::RemoveObserver(PrefStore::Observer* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  observers_.RemoveObserver(observer);
}

bool JsonPrefStore::HasObservers() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return observers_.might_have_observers();
}

bool JsonPrefStore::IsInitializationComplete() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return initialized_;
}

bool JsonPrefStore::GetMutableValue(const std::string& key,
                                    base::Value** result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return prefs_->Get(key, result);
}

void JsonPrefStore::SetValue(const std::string& key,
                             std::unique_ptr<base::Value> value,
                             uint32_t flags) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(value);
  base::Value* old_value = nullptr;
  prefs_->Get(key, &old_value);
  if (!old_value || !value->Equals(old_value)) {
    prefs_->Set(key, std::move(value));
    ReportValueChanged(key, flags);
  }
}

void JsonPrefStore::SetValueSilently(const std::string& key,
                                     std::unique_ptr<base::Value> value,
                                     uint32_t flags) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(value);
  base::Value* old_value = nullptr;
  prefs_->Get(key, &old_value);
  if (!old_value || !value->Equals(old_value)) {
    prefs_->Set(key, std::move(value));
    ScheduleWrite(flags);
  }
}

void JsonPrefStore::RemoveValue(const std::string& key, uint32_t flags) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (prefs_->RemovePath(key, nullptr))
    ReportValueChanged(key, flags);
}

void JsonPrefStore::RemoveValueSilently(const std::string& key,
                                        uint32_t flags) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  prefs_->RemovePath(key, nullptr);
  ScheduleWrite(flags);
}

bool JsonPrefStore::ReadOnly() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return read_only_;
}

PersistentPrefStore::PrefReadError JsonPrefStore::GetReadError() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  return read_error_;
}

PersistentPrefStore::PrefReadError JsonPrefStore::ReadPrefs() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  OnFileRead(ReadPrefsFromDisk(path_));
  return filtering_in_progress_ ? PREF_READ_ERROR_ASYNCHRONOUS_TASK_INCOMPLETE
                                : read_error_;
}

void JsonPrefStore::ReadPrefsAsync(ReadErrorDelegate* error_delegate) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  initialized_ = false;
  error_delegate_.reset(error_delegate);

  // Weakly binds the read task so that it doesn't kick in during shutdown.
  base::PostTaskAndReplyWithResult(
      file_task_runner_.get(), FROM_HERE, base::Bind(&ReadPrefsFromDisk, path_),
      base::Bind(&JsonPrefStore::OnFileRead, AsWeakPtr()));
}

void JsonPrefStore::CommitPendingWrite(base::OnceClosure done_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Schedule a write for any lossy writes that are outstanding to ensure that
  // they get flushed when this function is called.
  SchedulePendingLossyWrites();

  if (writer_.HasPendingWrite() && !read_only_)
    writer_.DoScheduledWrite();

  if (done_callback) {
    // Since disk operations occur on |file_task_runner_|, the reply of a task
    // posted to |file_task_runner_| will run after currently pending disk
    // operations. Also, by definition of PostTaskAndReply(), the reply will run
    // on the current sequence.
    file_task_runner_->PostTaskAndReply(
        FROM_HERE, base::BindOnce(&base::DoNothing), std::move(done_callback));
  }
}

void JsonPrefStore::SchedulePendingLossyWrites() {
  if (pending_lossy_write_)
    writer_.ScheduleWrite(this);
}

void JsonPrefStore::ReportValueChanged(const std::string& key, uint32_t flags) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (pref_filter_)
    pref_filter_->FilterUpdate(key);

  for (PrefStore::Observer& observer : observers_)
    observer.OnPrefValueChanged(key);

  ScheduleWrite(flags);
}

void JsonPrefStore::RunOrScheduleNextSuccessfulWriteCallback(
    bool write_success) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  has_pending_write_reply_ = false;
  if (!on_next_successful_write_reply_.is_null()) {
    base::Closure on_successful_write =
        std::move(on_next_successful_write_reply_);
    if (write_success) {
      on_successful_write.Run();
    } else {
      RegisterOnNextSuccessfulWriteReply(on_successful_write);
    }
  }
}

// static
void JsonPrefStore::PostWriteCallback(
    const base::Callback<void(bool success)>& on_next_write_callback,
    const base::Callback<void(bool success)>& on_next_write_reply,
    scoped_refptr<base::SequencedTaskRunner> reply_task_runner,
    bool write_success) {
  if (!on_next_write_callback.is_null())
    on_next_write_callback.Run(write_success);

  // We can't run |on_next_write_reply| on the current thread. Bounce back to
  // the |reply_task_runner| which is the correct sequenced thread.
  reply_task_runner->PostTask(FROM_HERE,
                              base::Bind(on_next_write_reply, write_success));
}

void JsonPrefStore::RegisterOnNextSuccessfulWriteReply(
    const base::Closure& on_next_successful_write_reply) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(on_next_successful_write_reply_.is_null());

  on_next_successful_write_reply_ = on_next_successful_write_reply;

  // If there are pending callbacks, avoid erasing them; the reply will be used
  // as we set |on_next_successful_write_reply_|. Otherwise, setup a reply with
  // an empty callback.
  if (!has_pending_write_reply_) {
    has_pending_write_reply_ = true;
    writer_.RegisterOnNextWriteCallbacks(
        base::Closure(),
        base::Bind(
            &PostWriteCallback, base::Callback<void(bool success)>(),
            base::Bind(&JsonPrefStore::RunOrScheduleNextSuccessfulWriteCallback,
                       AsWeakPtr()),
            base::SequencedTaskRunnerHandle::Get()));
  }
}

void JsonPrefStore::RegisterOnNextWriteSynchronousCallbacks(
    OnWriteCallbackPair callbacks) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  has_pending_write_reply_ = true;

  writer_.RegisterOnNextWriteCallbacks(
      callbacks.first,
      base::Bind(
          &PostWriteCallback, callbacks.second,
          base::Bind(&JsonPrefStore::RunOrScheduleNextSuccessfulWriteCallback,
                     AsWeakPtr()),
          base::SequencedTaskRunnerHandle::Get()));
}

void JsonPrefStore::ClearMutableValues() {
  NOTIMPLEMENTED();
}

void JsonPrefStore::OnFileRead(std::unique_ptr<ReadResult> read_result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  DCHECK(read_result);

  std::unique_ptr<base::DictionaryValue> unfiltered_prefs(
      new base::DictionaryValue);

  read_error_ = read_result->error;

  bool initialization_successful = !read_result->no_dir;

  if (initialization_successful) {
    switch (read_error_) {
      case PREF_READ_ERROR_ACCESS_DENIED:
      case PREF_READ_ERROR_FILE_OTHER:
      case PREF_READ_ERROR_FILE_LOCKED:
      case PREF_READ_ERROR_JSON_TYPE:
      case PREF_READ_ERROR_FILE_NOT_SPECIFIED:
        read_only_ = true;
        break;
      case PREF_READ_ERROR_NONE:
        DCHECK(read_result->value.get());
        unfiltered_prefs.reset(
            static_cast<base::DictionaryValue*>(read_result->value.release()));
        break;
      case PREF_READ_ERROR_NO_FILE:
        // If the file just doesn't exist, maybe this is first run.  In any case
        // there's no harm in writing out default prefs in this case.
      case PREF_READ_ERROR_JSON_PARSE:
      case PREF_READ_ERROR_JSON_REPEAT:
        break;
      case PREF_READ_ERROR_ASYNCHRONOUS_TASK_INCOMPLETE:
        // This is a special error code to be returned by ReadPrefs when it
        // can't complete synchronously, it should never be returned by the read
        // operation itself.
      case PREF_READ_ERROR_MAX_ENUM:
        NOTREACHED();
        break;
    }
  }

  if (pref_filter_) {
    filtering_in_progress_ = true;
    const PrefFilter::PostFilterOnLoadCallback post_filter_on_load_callback(
        base::Bind(
            &JsonPrefStore::FinalizeFileRead, AsWeakPtr(),
            initialization_successful));
    pref_filter_->FilterOnLoad(post_filter_on_load_callback,
                               std::move(unfiltered_prefs));
  } else {
    FinalizeFileRead(initialization_successful, std::move(unfiltered_prefs),
                     false);
  }
}

JsonPrefStore::~JsonPrefStore() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CommitPendingWrite(base::OnceClosure());
}

bool JsonPrefStore::SerializeData(std::string* output) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  pending_lossy_write_ = false;

  write_count_histogram_.RecordWriteOccured();

  if (pref_filter_) {
    OnWriteCallbackPair callbacks =
        pref_filter_->FilterSerializeData(prefs_.get());
    if (!callbacks.first.is_null() || !callbacks.second.is_null())
      RegisterOnNextWriteSynchronousCallbacks(callbacks);
  }

  JSONStringValueSerializer serializer(output);
  // Not pretty-printing prefs shrinks pref file size by ~30%. To obtain
  // readable prefs for debugging purposes, you can dump your prefs into any
  // command-line or online JSON pretty printing tool.
  serializer.set_pretty_print(false);
  bool success = serializer.Serialize(*prefs_);
  DCHECK(success);
  return success;
}

void JsonPrefStore::FinalizeFileRead(
    bool initialization_successful,
    std::unique_ptr<base::DictionaryValue> prefs,
    bool schedule_write) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  filtering_in_progress_ = false;

  if (!initialization_successful) {
    for (PrefStore::Observer& observer : observers_)
      observer.OnInitializationCompleted(false);
    return;
  }

  prefs_ = std::move(prefs);

  initialized_ = true;

  if (schedule_write)
    ScheduleWrite(DEFAULT_PREF_WRITE_FLAGS);

  if (error_delegate_ && read_error_ != PREF_READ_ERROR_NONE)
    error_delegate_->OnError(read_error_);

  for (PrefStore::Observer& observer : observers_)
    observer.OnInitializationCompleted(true);

  return;
}

void JsonPrefStore::ScheduleWrite(uint32_t flags) {
  if (read_only_)
    return;

  if (flags & LOSSY_PREF_WRITE_FLAG)
    pending_lossy_write_ = true;
  else
    writer_.ScheduleWrite(this);
}

// NOTE: This value should NOT be changed without renaming the histogram
// otherwise it will create incompatible buckets.
const int32_t
    JsonPrefStore::WriteCountHistogram::kHistogramWriteReportIntervalMins = 5;

JsonPrefStore::WriteCountHistogram::WriteCountHistogram(
    const base::TimeDelta& commit_interval,
    const base::FilePath& path)
    : WriteCountHistogram(
          commit_interval,
          path,
          std::unique_ptr<base::Clock>(new base::DefaultClock)) {}

JsonPrefStore::WriteCountHistogram::WriteCountHistogram(
    const base::TimeDelta& commit_interval,
    const base::FilePath& path,
    std::unique_ptr<base::Clock> clock)
    : commit_interval_(commit_interval),
      path_(path),
      clock_(clock.release()),
      report_interval_(
          base::TimeDelta::FromMinutes(kHistogramWriteReportIntervalMins)),
      last_report_time_(clock_->Now()),
      writes_since_last_report_(0) {}

JsonPrefStore::WriteCountHistogram::~WriteCountHistogram() {
  ReportOutstandingWrites();
}

void JsonPrefStore::WriteCountHistogram::RecordWriteOccured() {
  ReportOutstandingWrites();

  ++writes_since_last_report_;
}

void JsonPrefStore::WriteCountHistogram::ReportOutstandingWrites() {
  base::Time current_time = clock_->Now();
  base::TimeDelta time_since_last_report = current_time - last_report_time_;

  if (time_since_last_report <= report_interval_)
    return;

  // If the time since the last report exceeds the report interval, report all
  // the writes since the last report. They must have all occurred in the same
  // report interval.
  base::HistogramBase* histogram = GetHistogram();
  histogram->Add(writes_since_last_report_);

  // There may be several report intervals that elapsed that don't have any
  // writes in them. Report these too.
  int64_t total_num_intervals_elapsed =
      (time_since_last_report / report_interval_);
  for (int64_t i = 0; i < total_num_intervals_elapsed - 1; ++i)
    histogram->Add(0);

  writes_since_last_report_ = 0;
  last_report_time_ += total_num_intervals_elapsed * report_interval_;
}

base::HistogramBase* JsonPrefStore::WriteCountHistogram::GetHistogram() {
  std::string spaceless_basename;
  base::ReplaceChars(path_.BaseName().MaybeAsASCII(), " ", "_",
                     &spaceless_basename);
  std::string histogram_name =
      "Settings.JsonDataWriteCount." + spaceless_basename;

  // The min value for a histogram is 1. The max value is the maximum number of
  // writes that can occur in the window being recorded. The number of buckets
  // used is the max value (plus the underflow/overflow buckets).
  int32_t min_value = 1;
  int32_t max_value = report_interval_ / commit_interval_;
  int32_t num_buckets = max_value + 1;

  // NOTE: These values should NOT be changed without renaming the histogram
  // otherwise it will create incompatible buckets.
  DCHECK_EQ(30, max_value);
  DCHECK_EQ(31, num_buckets);

  // The histogram below is an expansion of the UMA_HISTOGRAM_CUSTOM_COUNTS
  // macro adapted to allow for a dynamically suffixed histogram name.
  // Note: The factory creates and owns the histogram.
  base::HistogramBase* histogram = base::Histogram::FactoryGet(
      histogram_name, min_value, max_value, num_buckets,
      base::HistogramBase::kUmaTargetedHistogramFlag);
  return histogram;
}
