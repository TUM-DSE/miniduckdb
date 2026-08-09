/*
 * duckdb::LocalFileSystem, implemented against modules/miniext.
 *
 * This *replaces* upstream's src/common/local_file_system.cpp, which is
 * excluded from the build (see miniosv/gen-sources.py). On miniOSv miniext is
 * the local filesystem -- there is no other -- so implementing the class DuckDB
 * already reaches for beats adding a parallel one beside it.
 *
 * Doing it this way means everything downstream works without being told:
 * FileSystem::CreateLocal() returns this, the default DBConfig
 * (VirtualFileSystem over CreateLocal()) is already correct, and the benchmark
 * runner's four CreateLocal() call sites -- which are static and could not have
 * been redirected -- pick it up for free.
 *
 * libc's open()/read()/stat() still fail with ENOENT exactly as before. Nothing
 * here goes through them; every call lands in miniext, which drives NVMe
 * itself.
 *
 * The declaration in src/include/duckdb/common/local_file_system.hpp is
 * unmodified, so every method it declares is defined below -- including the
 * ones that cannot mean anything here, which fail or return a neutral value
 * rather than pretending.
 */

#include "duckdb/common/local_file_system.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"

#include "modules/miniext/miniext.hh"

#include <cerrno>

namespace duckdb {

namespace {

// The handle owns a miniext::file. FileHandle::file_system points back at the
// filesystem that opened it, so every later operation re-enters this class
// rather than the VirtualFileSystem wrapper.
class MiniextFileHandle : public FileHandle {
public:
	MiniextFileHandle(FileSystem &fs, string path_p, miniext::file *file_p, FileOpenFlags flags)
	    : FileHandle(fs, std::move(path_p), flags), file(file_p) {
	}
	~MiniextFileHandle() override {
		MiniextFileHandle::Close();
	}

	void Close() override {
		if (file) {
			miniext::close(file);
			file = nullptr;
		}
	}

	miniext::file *file;
	//! Sequential Read/Write use this; the positional forms do not touch it.
	idx_t position = 0;
};

//! Named to avoid colliding with FileHandle::Cast<T>().
MiniextFileHandle &AsMiniext(FileHandle &handle) {
	return handle.Cast<MiniextFileHandle>();
}

//! miniext returns -errno. Name the file in the exception: "IO Error" with no
//! path is close to useless in a log.
[[noreturn]] void ThrowIO(const string &what, const string &path, int64_t rc) {
	throw IOException("miniext: %s(\"%s\") failed with errno %d", what, path,
	                  static_cast<int>(-rc));
}

//! There is no working directory on miniOSv -- getcwd() reports "/" and chdir()
//! fails -- so a relative path is resolved against the mount point instead.
string Absolute(const string &path) {
	if (!path.empty() && path[0] == '/') {
		return path;
	}
	string root = miniext::mount_point();
	if (root.empty()) {
		return path;
	}
	return root + "/" + path;
}

} // namespace

unique_ptr<FileHandle> LocalFileSystem::OpenFile(const string &path_p, FileOpenFlags flags,
                                                 optional_ptr<FileOpener> opener) {
	const string path = Absolute(path_p);

	int mode = 0;
	if (flags.OpenForReading()) {
		mode |= miniext::O_RD;
	}
	if (flags.OpenForWriting()) {
		mode |= miniext::O_WR;
	}
	// Mirrors upstream's GetFileFlags: FILE_CREATE means "create if absent",
	// FILE_CREATE_NEW means "create and truncate" -- not O_EXCL.
	if (flags.CreateFileIfNotExists()) {
		mode |= miniext::O_CREATE;
	} else if (flags.OverwriteExistingFile()) {
		mode |= miniext::O_CREATE | miniext::O_TRUNC;
	}

	// flags.Lock() is deliberately ignored. DuckDB takes fcntl range locks on
	// the database file itself rather than creating lock files, nothing
	// verifies the lock was taken, and this kernel runs one process.

	int err = 0;
	miniext::file *file = miniext::open(path.c_str(), mode, &err);
	if (!file) {
		if (err == -ENOENT && flags.ReturnNullIfNotExists()) {
			return nullptr;
		}
		ThrowIO("open", path, err);
	}

	auto handle = make_uniq<MiniextFileHandle>(*this, path, file, flags);
	if (flags.OpenForAppending()) {
		handle->position = miniext::size(file);
	}
	return std::move(handle);
}

void LocalFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	int64_t got = miniext::pread(AsMiniext(handle).file, buffer, static_cast<size_t>(nr_bytes), location);
	if (got < 0) {
		ThrowIO("pread", handle.path, got);
	}
	if (got != nr_bytes) {
		// Positional reads are all-or-nothing: DuckDB reads whole blocks, and a
		// short read means the file is not what the storage layer believes.
		throw IOException("miniext: read %lld of %lld bytes at %llu in \"%s\"",
		                  static_cast<long long>(got), static_cast<long long>(nr_bytes),
		                  static_cast<unsigned long long>(location), handle.path);
	}
}

void LocalFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	int64_t put = miniext::pwrite(AsMiniext(handle).file, buffer, static_cast<size_t>(nr_bytes), location);
	if (put < 0) {
		ThrowIO("pwrite", handle.path, put);
	}
	if (put != nr_bytes) {
		throw IOException("miniext: wrote %lld of %lld bytes at %llu in \"%s\"",
		                  static_cast<long long>(put), static_cast<long long>(nr_bytes),
		                  static_cast<unsigned long long>(location), handle.path);
	}
}

int64_t LocalFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &fh = AsMiniext(handle);
	int64_t got = miniext::pread(fh.file, buffer, static_cast<size_t>(nr_bytes), fh.position);
	if (got < 0) {
		ThrowIO("read", handle.path, got);
	}
	fh.position += static_cast<idx_t>(got);
	return got;
}

int64_t LocalFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &fh = AsMiniext(handle);
	int64_t put = miniext::pwrite(fh.file, buffer, static_cast<size_t>(nr_bytes), fh.position);
	if (put < 0) {
		ThrowIO("write", handle.path, put);
	}
	fh.position += static_cast<idx_t>(put);
	return put;
}

//! Punching holes is not implemented; the base class treats false as "the range
//! still holds its old contents", which is true.
bool LocalFileSystem::Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) {
	return false;
}

int64_t LocalFileSystem::GetFileSize(FileHandle &handle) {
	return static_cast<int64_t>(miniext::size(AsMiniext(handle).file));
}

//! miniext does not keep modification times: the on-disk inode has the fields,
//! but nothing sets them and there is no wall clock at mount time worth
//! trusting. Report the epoch rather than inventing a value.
timestamp_t LocalFileSystem::GetLastModifiedTime(FileHandle &handle) {
	return timestamp_t(0);
}

string LocalFileSystem::GetVersionTag(FileHandle &handle) {
	return string();
}

FileType LocalFileSystem::GetFileType(FileHandle &handle) {
	return miniext::is_directory(handle.path.c_str()) ? FileType::FILE_TYPE_DIR
	                                                  : FileType::FILE_TYPE_REGULAR;
}

FileMetadata LocalFileSystem::Stats(FileHandle &handle) {
	FileMetadata result;
	result.file_size = GetFileSize(handle);
	result.last_modification_time = timestamp_t(0);
	result.file_type = GetFileType(handle);
	return result;
}

void LocalFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	int rc = miniext::truncate(AsMiniext(handle).file, static_cast<uint64_t>(new_size));
	if (rc < 0) {
		ThrowIO("truncate", handle.path, rc);
	}
}

bool LocalFileSystem::DirectoryExists(const string &directory, optional_ptr<FileOpener> opener) {
	return miniext::is_directory(Absolute(directory).c_str());
}

void LocalFileSystem::CreateDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	const string path = Absolute(directory);
	if (miniext::is_directory(path.c_str())) {
		return; // DuckDB calls this without checking first
	}
	int rc = miniext::mkdir(path.c_str());
	if (rc < 0 && rc != -EEXIST) {
		ThrowIO("mkdir", path, rc);
	}
}

void LocalFileSystem::RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener) {
	// DuckDB expects this to remove the tree; miniext::rmdir only removes an
	// empty directory, so recurse first. Temp directories are what reach here.
	const string path = Absolute(directory);
	if (!miniext::is_directory(path.c_str())) {
		return;
	}
	vector<string> files;
	vector<string> dirs;
	miniext::list(path.c_str(), [&](const char *name, bool is_dir) {
		(is_dir ? dirs : files).push_back(path + "/" + name);
	});
	for (auto &f : files) {
		miniext::unlink(f.c_str());
	}
	for (auto &d : dirs) {
		RemoveDirectory(d, opener);
	}
	int rc = miniext::rmdir(path.c_str());
	if (rc < 0) {
		ThrowIO("rmdir", path, rc);
	}
}

void LocalFileSystem::MoveFile(const string &source, const string &target,
                               optional_ptr<FileOpener> opener) {
	int rc = miniext::rename(Absolute(source).c_str(), Absolute(target).c_str());
	if (rc < 0) {
		ThrowIO("rename", source, rc);
	}
}

bool LocalFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	return miniext::exists(Absolute(filename).c_str());
}

//! There are no FIFOs: miniext stores regular files and directories only.
bool LocalFileSystem::IsPipe(const string &filename, optional_ptr<FileOpener> opener) {
	return false;
}

void LocalFileSystem::RemoveFile(const string &filename, optional_ptr<FileOpener> opener) {
	const string path = Absolute(filename);
	int rc = miniext::unlink(path.c_str());
	if (rc < 0) {
		ThrowIO("unlink", path, rc);
	}
}

void LocalFileSystem::FileSync(FileHandle &handle) {
	int rc = miniext::sync(AsMiniext(handle).file);
	if (rc < 0) {
		ThrowIO("sync", handle.path, rc);
	}
}

bool LocalFileSystem::IsPathAbsolute(const string &path) {
	return !path.empty() && path[0] == '/';
}

string LocalFileSystem::MakePathAbsolute(const string &input, optional_ptr<FileOpener> opener) {
	return Absolute(input);
}

//! No drive letters: this is not Windows.
bool LocalFileSystem::PathStartsWithDrive(const string &path) {
	return false;
}

void LocalFileSystem::Seek(FileHandle &handle, idx_t location) {
	AsMiniext(handle).position = location;
}

idx_t LocalFileSystem::SeekPosition(FileHandle &handle) {
	return AsMiniext(handle).position;
}

bool LocalFileSystem::CanSeek() {
	return true;
}

bool LocalFileSystem::OnDiskFile(FileHandle &handle) {
	return true;
}

//! Windows-only in upstream; there is no error string to report here.
std::string LocalFileSystem::GetLastErrorAsString() {
	return std::string();
}

//! miniext does not implement permissions, so every file is as private as any
//! other. Saying "yes" keeps DuckDB from refusing to use its own files.
bool LocalFileSystem::IsPrivateFile(const string &path_p, FileOpener *opener) {
	return true;
}

vector<OpenFileInfo> LocalFileSystem::FetchFileWithoutGlob(const string &path,
                                                           optional_ptr<FileOpener> opener,
                                                           bool absolute_path) {
	vector<OpenFileInfo> result;
	const string full = absolute_path ? path : Absolute(path);
	if (miniext::exists(full.c_str())) {
		result.emplace_back(full);
	}
	return result;
}

//! Pure string work: there are no symlinks and no working directory to resolve
//! against, so the absolute form is already canonical.
string LocalFileSystem::CanonicalizePath(const string &path_p, optional_ptr<FileOpener> opener) {
	return Absolute(path_p);
}

bool LocalFileSystem::TryCanonicalizeExistingPath(string &path_p) {
	const string full = Absolute(path_p);
	if (!miniext::exists(full.c_str())) {
		return false;
	}
	path_p = full;
	return true;
}

bool LocalFileSystem::ListFilesExtended(const string &directory,
                                        const std::function<void(OpenFileInfo &info)> &callback,
                                        optional_ptr<FileOpener> opener) {
	const string path = Absolute(directory);
	if (!miniext::is_directory(path.c_str())) {
		return false;
	}
	bool any = false;
	int rc = miniext::list(path.c_str(), [&](const char *name, bool is_dir) {
		any = true;
		OpenFileInfo info{string(name)};
		callback(info);
	});
	return rc < 0 ? false : any;
}

unique_ptr<MultiFileList> LocalFileSystem::GlobFilesExtended(const string &path,
                                                             const FileGlobInput &input,
                                                             optional_ptr<FileOpener> opener) {
	// Upstream expands lazily; miniext has no pattern matching, so the result is
	// either the one path that exists or nothing. Anything with a wildcard is
	// reported rather than silently matching nothing -- the benchmark runner
	// globs a directory of .benchmark files and needs to know if that fails.
	vector<OpenFileInfo> files;
	if (FileSystem::HasGlob(path)) {
		throw NotImplementedException("miniext: glob patterns are not supported (\"%s\")", path);
	}
	const string full = Absolute(path);
	if (miniext::exists(full.c_str())) {
		files.emplace_back(full);
	}
	return make_uniq<SimpleMultiFileList>(std::move(files));
}

void LocalFileSystem::SetFilePointer(FileHandle &handle, idx_t location) {
	AsMiniext(handle).position = location;
}

idx_t LocalFileSystem::GetFilePointer(FileHandle &handle) {
	return AsMiniext(handle).position;
}

unique_ptr<FileSystem> FileSystem::CreateLocal() {
	return make_uniq<LocalFileSystem>();
}

} // namespace duckdb
