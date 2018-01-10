#include <exceptions.h>
#include <file.h>
#include <logger.h>
#include <sys/sendfile.h>
#include <sstream>

off_t file::File::Size() const { return Status().st_size; }

mode_t file::File::Mode() const { return Status().st_mode; }

bool file::File::Remove(bool silent) {
  DEFER(Invalidate());

  if (unlink(path_.c_str()) == 0) {
    return true;
  }

  if (errno == ENOENT && silent) {
    return false;
  }

  throw suex::IOError("%s: %s", path_.c_str(), std::strerror(errno));
}

bool file::File::IsSecure() const {
  const stat_t st = Status();
  int perms = st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO);
  return perms != (S_IRUSR | S_IRGRP) || st.st_uid != 0 || st.st_gid == 0;
}
off_t file::File::Tell() const { return Seek(0, SEEK_CUR); }

void file::File::Close() {
  if (!Valid()) {
    return;
  }
  DEFER(Invalidate());
  if (close(fd_) < 0) {
    throw suex::IOError("error closing '%d': %s", fd_, std::strerror(errno));
  }
  logger::debug() << "closed fd: " << fd_ << std::endl;
}
void file::File::Clone(file::File &other, mode_t mode) const {
  // only secure the other if it should be secured

  const stat_t st = Status();
  logger::debug() << "cloning " << path_ << "(" << st.st_size << " bytes) -> "
                  << other.path_ << std::endl;

  /* seek to the beginning of the other,
   * but don't forget to go back to the previous location */
  off_t src_pos = Tell();
  DEFER(Seek(src_pos, SEEK_SET));
  Seek(0, SEEK_SET);

  off_t dst_pos = other.Tell();
  DEFER(other.Seek(dst_pos, SEEK_SET));
  other.Seek(0, SEEK_SET);

  if (sendfile(other.fd_, fd_, nullptr, static_cast<size_t>(st.st_size)) < 0) {
    throw suex::IOError("can't clone '%d to '%d'. sendfile() failed: %s", fd_,
                        other.fd_, std::strerror(errno));
  }

  if (fchown(other.fd_, st.st_uid, st.st_gid) < 0) {
    throw suex::PermissionError("error on chown '%d': %s", other.fd_,
                                std::strerror(errno));
  }

  if (fchmod(other.fd_, mode) < 0) {
    throw suex::PermissionError("error on chmod '%d': %s", other.fd_,
                                std::strerror(errno));
  }
}
off_t file::File::Seek(off_t offset, int whence) const {
  off_t pos = lseek(fd_, offset, whence);
  if (pos < 0) {
    throw suex::IOError("error seeking '%d': %s", fd_, std::strerror(errno));
  }
  return pos;
}
ssize_t file::File::Read(gsl::span<char> buff) const {
  ssize_t bytes = read(fd_, buff.data(), static_cast<size_t>(buff.size()));
  if (bytes == -1) {
    throw suex::IOError("couldn't read from fd '%d: '", fd_, strerror(errno));
  }
  return bytes;
}
ssize_t file::File::Write(gsl::span<const char> buff) const {
  ssize_t bytes = write(fd_, buff.data(), static_cast<size_t>(buff.size()));
  if (bytes == -1) {
    throw suex::IOError("couldn't write to fd '%d: %s'", fd_, strerror(errno));
  }
  return bytes;
}

const std::string &file::File::Path() const { return path_; }

const std::string &file::File::DescriptorPath() const { return internal_path_; }

file::File::File(int fd)
    : fd_{fd},
      path_{utils::path::Readlink(fd)},
      internal_path_{utils::path::GetPath(fd)} {}

bool file::File::Valid() const {
  if (fd_ == -1) {
    return false;
  }

  // if fd > 0, really check the fd
  return fcntl(fd_, F_GETFD) == 0;
}

file::File::File(file::File &other) noexcept {
  fd_ = other.fd_;
  path_ = other.path_;
  internal_path_ = other.internal_path_;
  auto_close_ = other.auto_close_;
}

void file::File::SuppressClose() {
  // doesn't need to close
  auto_close_ = false;
}

std::string file::File::String() const {
  std::stringstream ss;
  ss << Path() << "' (fd " << fd_ << ")";
  return ss.str();
}
void file::File::ReadLine(
    std::function<void(const file::line_t &)> &&callback) {
  // fdopen should have seeked to the beginning of the file
  // but in practice, it doesn't always work.
  off_t fd_pos = Tell();
  DEFER(Seek(fd_pos, SEEK_SET));
  Seek(0, SEEK_SET);

  FILE *f = fdopen(fd_, "r");
  if (f == nullptr) {
    throw suex::IOError("error opening '%d' for reading: %s", fd_,
                        std::strerror(errno));
  }
  size_t len{0};
  ssize_t read{0};

  char *line = nullptr;
  for (int lineno = 1; (read = getline(&line, &len, f)) != -1; lineno++) {
    if (line[read - 1] == '\n') {
      line[read - 1] = '\0';
    }

    callback(line_t{.lineno = lineno, .txt = line});
  }
}
file::File::~File() {
  if (Valid() && !auto_close_) {
    Close();
  }
}

const file::stat_t file::File::Status() const {
  stat_t st{0};
  if (fstat(fd_, &st) != 0) {
    throw IOError("could not get file %d status: %s", fd_, strerror(errno));
  }
  return st;
}
void file::File::Invalidate() { fd_ = -1; }
