// Apps from the store (iotstore): a package (.idbz, tools/idbz.py) is written to the card as it
// downloads, file by file, each checked against the SHA-256 in the package's table, into a hidden
// staging folder (/applications/.store-<id>). When every file is there and right, they replace the
// app's files (its memory.json and data/ stay) and install.json says where the app came from.
//
// Script (system apps only, os.store in os.js), answered through the os.net request channel:
//   store.install  {id, url, version?, version_id?, store?, size?}  -> {id, version}
//   store.download {url, path}  a file under /system/data/store/ (icons)  -> {path, size}
//   store.cancel   {id}
// Bus event "install": {id, state: "downloading" | "installed" | "failed", got, total, error}

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "js.h"
#include "os.h"
#include "sha256.h"

namespace rt {

namespace {

bool okName(const std::string &n) {
  if (n.empty() || n.size() > 23 || n[0] == '.') return false;
  for (char c : n)
    if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
  return true;
}

bool okId(const std::string &id) {
  if (id.empty() || id.size() > 64 || !isalnum((unsigned char)id[0])) return false;
  for (char c : id)
    if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '-')) return false;
  return true;
}

uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

}  // namespace

// Writes an IDBZ package into a directory as its bytes arrive (any piece sizes).
class IdbzWriter {
 public:
  explicit IdbzWriter(std::string dir) : dir_(std::move(dir)) {}
  ~IdbzWriter() {
    if (f_) fclose(f_);
  }

  bool feed(const uint8_t *p, size_t n) {
    if (!err_.empty()) return false;
    while (n > 0) {
      if (st_ != BODY) {
        size_t want = head_.size() < 64 ? 64 : need_;
        size_t take = std::min(n, want - head_.size());
        head_.append((const char *)p, take);
        p += take;
        n -= take;
        pos_ += take;
        if (head_.size() == 64 && st_ == HEAD && !parseHeader()) return false;
        if (st_ == TABLE && head_.size() == need_ && !parseTable()) return false;
        continue;
      }
      if (cur_ >= entries_.size()) {  // past the last file: nothing more belongs to the package
        pos_ += n;
        break;
      }
      Entry &e = entries_[cur_];
      if (pos_ < e.off) {  // padding before the file
        size_t skip = (size_t)std::min<uint64_t>(n, e.off - pos_);
        p += skip;
        n -= skip;
        pos_ += skip;
        continue;
      }
      if (!startEntry()) return false;
      size_t take = std::min<size_t>(n, e.size - written_);
      if (f_ && fwrite(p, 1, take, f_) != take) return fail("cannot write " + e.name + " (card full?)");
      sha_.update(p, take);
      written_ += (uint32_t)take;
      p += take;
      n -= take;
      pos_ += take;
      if (written_ == e.size && !endEntry()) return false;
    }
    return finishEmpty();
  }

  // every file written and verified, the whole package seen
  bool complete() {
    finishEmpty();
    return err_.empty() && st_ == BODY && cur_ == entries_.size() && pos_ >= total_;
  }
  const std::string &error() const { return err_; }
  const Json &manifest() const { return manifest_; }
  uint32_t total() const { return total_; }

 private:
  struct Entry {
    std::string name;
    uint32_t off, size;
    uint8_t sha[32];
  };
  enum { HEAD, TABLE, BODY } st_ = HEAD;

  bool fail(const std::string &m) {
    if (err_.empty()) err_ = m;
    if (f_) {
      fclose(f_);
      f_ = nullptr;
    }
    return false;
  }

  bool parseHeader() {
    const uint8_t *h = (const uint8_t *)head_.data();
    if (memcmp(h, "IDBZ", 4) != 0) return fail("not an app package (IDBZ)");
    if (le16(h + 4) != 1) return fail("package format " + std::to_string(le16(h + 4)) + " is too new for this OS");
    count_ = le16(h + 6);
    moff_ = le32(h + 8);
    mlen_ = le32(h + 12);
    total_ = le32(h + 16);
    memcpy(msha_, h + 20, 32);
    if (count_ == 0 || count_ > 64 || 64u + 64u * count_ > moff_ || mlen_ > 64 * 1024 || moff_ + mlen_ > total_ ||
        total_ > 64u * 1024 * 1024)
      return fail("damaged package header");
    need_ = moff_ + mlen_;
    st_ = TABLE;
    return true;
  }

  bool parseTable() {
    const uint8_t *t = (const uint8_t *)head_.data();
    for (uint32_t i = 0; i < count_; i++) {
      const uint8_t *e = t + 64 + 64 * i;
      Entry x;
      x.name.assign((const char *)e, strnlen((const char *)e, 24));
      x.off = le32(e + 24);
      x.size = le32(e + 28);
      memcpy(x.sha, e + 32, 32);
      if (!okName(x.name)) return fail("bad file name in the package");
      if (x.off < moff_ + mlen_ || (uint64_t)x.off + x.size > total_) return fail(x.name + " lies outside the package");
      entries_.push_back(x);
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) { return a.off < b.off; });
    for (size_t i = 1; i < entries_.size(); i++)
      if (entries_[i].off < entries_[i - 1].off + entries_[i - 1].size) return fail("files overlap in the package");
    for (const char *must : {"app.json", "icon.img", "app.idb"})
      if (std::none_of(entries_.begin(), entries_.end(), [&](const Entry &e) { return e.name == must; }))
        return fail(std::string("the package has no ") + must);
    Sha256 s;
    s.update(head_.data() + moff_, mlen_);
    uint8_t h[32];
    s.final(h);
    if (memcmp(h, msha_, 32) != 0) return fail("package description checksum mismatch");
    if (!Json::parse(head_.data() + moff_, mlen_, manifest_) || manifest_.type != Json::Obj)
      return fail("bad package description");
    st_ = BODY;
    return true;
  }

  bool startEntry() {
    if (opened_) return true;
    Entry &e = entries_[cur_];
    f_ = fopen((dir_ + "/" + e.name).c_str(), "wb");
    if (!f_) return fail("cannot create " + e.name);
    sha_.reset();
    written_ = 0;
    opened_ = true;
    return true;
  }

  bool endEntry() {
    Entry &e = entries_[cur_];
    uint8_t h[32];
    sha_.final(h);
    if (f_ && fclose(f_) != 0) {
      f_ = nullptr;
      return fail("cannot write " + e.name);
    }
    f_ = nullptr;
    if (memcmp(h, e.sha, 32) != 0) return fail(e.name + ": checksum mismatch (damaged download)");
    cur_++;
    opened_ = false;
    return true;
  }

  // files of size 0 have no bytes to wait for
  bool finishEmpty() {
    while (err_.empty() && st_ == BODY && cur_ < entries_.size() && entries_[cur_].size == 0 && pos_ >= entries_[cur_].off) {
      if (!startEntry() || !endEntry()) return false;
    }
    return err_.empty();
  }

  std::string dir_, head_, err_;
  uint32_t count_ = 0, moff_ = 0, mlen_ = 0, total_ = 0;
  size_t need_ = 0;
  uint8_t msha_[32];
  Json manifest_;
  std::vector<Entry> entries_;
  size_t cur_ = 0;
  uint64_t pos_ = 0;
  FILE *f_ = nullptr;
  Sha256 sha_;
  uint32_t written_ = 0;
  bool opened_ = false;
};

struct Os::StoreJob {
  Os *os = nullptr;  // cleared when the OS goes away first
  bool install = true;
  std::string id, url, version, versionId, store, stage, path, tmp;
  std::unique_ptr<IdbzWriter> writer;
  FILE *file = nullptr;
  std::atomic<uint64_t> got{0}, total{0};
  std::atomic<bool> cancel{false};
  std::string sinkErr;  // the sink's own error (a write failed)
  Platform::RadioDone done;
  double lastEmit = 0;
  uint64_t lastGot = 0;
  ~StoreJob() {
    if (file) fclose(file);
  }
};

static std::string installEvent(const std::string &id, const std::string &state, uint64_t got, uint64_t total,
                                const std::string &err) {
  return "{\"id\":" + Json::quote(id) + ",\"state\":" + Json::quote(state) + ",\"got\":" + std::to_string(got) +
         ",\"total\":" + std::to_string(total) + (err.empty() ? "" : ",\"error\":" + Json::quote(err)) + "}";
}

void Os::storeOp(const std::string &op, const Json &args, Platform::RadioDone done) {
  if (op == "store.cancel") {
    std::string id = args.strOr("id", "");
    for (auto &j : storeJobs_)
      if (j->id == id) j->cancel = true;
    return done("", "true");
  }
  std::string root = plat_.sdRoot();
  if (root.empty()) return done("no storage", "");
  auto job = std::make_shared<StoreJob>();
  job->os = this;
  job->url = args.strOr("url", "");
  if (job->url.rfind("http://", 0) != 0 && job->url.rfind("https://", 0) != 0) return done("bad url", "");
  job->done = std::move(done);

  if (op == "store.download") {  // a small file the store app keeps (icons)
    std::string path = args.strOr("path", "");
    if (path.rfind("/system/data/store/", 0) != 0 || path.find("..") != std::string::npos)
      return job->done("files go under /system/data/store/", "");
    job->install = false;
    job->id = path;
    job->path = hostPath(path);
    job->tmp = job->path + ".part";
    size_t slash = job->path.rfind('/');
    makeDirs(job->path.substr(0, slash));
    job->file = fopen(job->tmp.c_str(), "wb");
    if (!job->file) return job->done("cannot create " + path, "");
  } else if (op == "store.install") {
    job->id = args.strOr("id", "");
    if (!okId(job->id)) return job->done("bad app id", "");
    for (auto &j : storeJobs_)
      if (j->install && j->id == job->id) return job->done("already installing " + job->id, "");
    if (app_.id == job->id) return job->done("the app is open: close it first", "");
    job->version = args.strOr("version", "");
    job->versionId = args.strOr("version_id", "");
    job->store = args.strOr("store", "");
    double size = args.numOr("size", 0);
    SdInfo info = plat_.sdInfo();
    if (size > 0 && info.mounted && info.freeBytes && info.freeBytes < size + 256 * 1024)
      return job->done("not enough space for " + std::to_string((int)(size / 1024)) + " KB", "");
    job->stage = root + "/applications/.store-" + job->id;
    removeTree(job->stage);
    if (!makeDirs(job->stage)) return job->done("cannot create " + job->stage, "");
    job->writer.reset(new IdbzWriter(job->stage));
  } else {
    return job->done("store: unknown operation " + op, "");
  }

  storeJobs_.push_back(job);
  if (job->install) js_.busEmit("install", installEvent(job->id, "downloading", 0, 0, ""));
  std::weak_ptr<StoreJob> weak = job;
  plat_.download(
      job->url,
      // worker thread: only this job's writer / file and its atomics
      [job](const uint8_t *data, size_t len, uint64_t total) {
        if (job->cancel) return false;
        if (total) job->total = total;
        job->got += len;
        if (job->writer) return job->writer->feed(data, len);
        if (fwrite(data, 1, len, job->file) != len) {
          job->sinkErr = "write failed (storage full?)";
          return false;
        }
        return true;
      },
      [this, weak](const std::string &err, int status) {
        auto job = weak.lock();
        if (!job || !job->os) return;
        storeJobs_.erase(std::remove(storeJobs_.begin(), storeJobs_.end(), job), storeJobs_.end());
        std::string e = job->cancel ? "cancelled" : job->writer && !job->writer->error().empty() ? job->writer->error()
                        : !job->sinkErr.empty() ? job->sinkErr : err;
        if (e.empty() && (status < 200 || status >= 300)) e = "the server answered " + std::to_string(status);
        if (!job->install) {
          fclose(job->file);
          job->file = nullptr;
          if (e.empty() && rename(job->tmp.c_str(), job->path.c_str()) != 0) {
            remove(job->path.c_str());
            if (rename(job->tmp.c_str(), job->path.c_str()) != 0) e = "cannot save " + job->id;
          }
          if (!e.empty()) remove(job->tmp.c_str());
          return job->done(e, e.empty() ? "{\"path\":" + Json::quote(job->id) + ",\"size\":" + std::to_string((uint64_t)job->got) + "}" : "");
        }
        if (e.empty() && !job->writer->complete()) e = job->writer->error().empty() ? "the download ended early" : job->writer->error();
        if (e.empty()) commitInstall(*job, e);
        removeTree(job->stage);
        js_.busEmit("install", installEvent(job->id, e.empty() ? "installed" : "failed", job->got, job->total, e));
        if (!e.empty()) return job->done(e, "");
        appsChanged(job->id);
        job->done("", "{\"id\":" + Json::quote(job->id) + ",\"version\":" + Json::quote(job->version) + "}");
      });
}

// The staged files replace the app's (memory.json and data/ stay); install.json records the source.
bool Os::commitInstall(StoreJob &job, std::string &err) {
  const Json &m = job.writer->manifest();
  std::string id = m.strOr("id", "");
  if (id != job.id) {
    err = "the package is the app '" + id + "', not '" + job.id + "'";
    return false;
  }
  std::string meta;
  Json app;
  if (!readFile(job.stage + "/app.json", meta) || !Json::parse(meta.data(), meta.size(), app) || app.strOr("id", "") != id) {
    err = "the package's app.json does not match it";
    return false;
  }
  if (app_.id == id) {
    err = "the app is open: close it first";
    return false;
  }
  if (job.version.empty()) job.version = m.strOr("version", "");
  std::string dir = plat_.sdRoot() + "/applications/" + id;
  if (!makeDirs(dir + "/data")) {
    err = "cannot create " + dir;
    return false;
  }
  if (DIR *d = opendir(job.stage.c_str())) {
    std::vector<std::string> names;
    while (dirent *e = readdir(d))
      if (e->d_name[0] != '.') names.push_back(e->d_name);
    closedir(d);
    // app.idb last: an app whose image is missing does not start half replaced
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
      return (a == "app.idb") < (b == "app.idb") || ((a == "app.idb") == (b == "app.idb") && a < b);
    });
    for (auto &n : names) {
      std::string to = dir + "/" + n;
      remove(to.c_str());
      if (rename((job.stage + "/" + n).c_str(), to.c_str()) != 0) {
        err = "cannot move " + n + " into place";
        return false;
      }
    }
  }
  char when[24];
  snprintf(when, sizeof when, "%lld", (long long)time(nullptr));
  writeFile(dir + "/install.json", "{\"source\":\"store\",\"version\":" + Json::quote(job.version) +
                                       ",\"version_id\":" + Json::quote(job.versionId) + ",\"store\":" + Json::quote(job.store) +
                                       ",\"installed\":" + when + "}");
  return true;
}

void Os::storeStop() {
  for (auto &j : storeJobs_) {
    j->cancel = true;
    j->os = nullptr;
  }
  storeJobs_.clear();
}

void Os::storeTick() {
  for (auto &j : storeJobs_) {
    if (!j->install) continue;
    uint64_t got = j->got;
    if (got != j->lastGot && now_ - j->lastEmit >= 150) {
      j->lastGot = got;
      j->lastEmit = now_;
      js_.busEmit("install", installEvent(j->id, "downloading", got, j->total, ""));
    }
  }
}

}  // namespace rt
