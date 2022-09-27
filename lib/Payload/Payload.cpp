//===- Payload.cpp ----------------------------------------------*- C++ -*-===//
//
// (C) Copyright IBM 2021, 2022.
//
// Any modifications or derivative works of this code must retain this
// copyright notice, and modified files need to carry a notice indicating
// that they have been altered from the originals.
//
//===----------------------------------------------------------------------===//
//
// Implements the Payload wrapper class
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <ostream>
#include <sys/stat.h>
#include <unordered_set>

#include "nlohmann/json.hpp"
#include <zip.h>

#include "Config.h"
#include "Payload/Payload.h"

using namespace qssc::payload;
namespace fs = std::filesystem;

auto Payload::getFile(const std::string &fName) -> std::string * {
  std::lock_guard<std::mutex> lock(_mtx);
  std::string key = prefix + fName;
  files.try_emplace(key);
  return &files[key];
}

auto Payload::getFile(const char *fName) -> std::string * {
  std::lock_guard<std::mutex> lock(_mtx);
  std::string key = prefix + fName;
  files.try_emplace(key);
  return &files[key];
}

auto Payload::orderedFileNames() -> std::vector<fs::path> {
  std::lock_guard<std::mutex> lock(_mtx);
  std::vector<fs::path> ret;
  for (auto &filePair : files)
    ret.emplace_back(filePair.first);
  std::sort(ret.begin(), ret.end());
  return ret;
}

// creates a manifest json file and adds it to the file map
void ZipPayload::addManifest() {
  std::lock_guard<std::mutex> lock(_mtx);
  std::string manifest_fname = "manifest/manifest.json";
  nlohmann::json manifest;
  manifest["version"] = QSSC_VERSION;
  manifest["contents_path"] = prefix;
  files[manifest_fname] = manifest.dump() + "\n";
}

void ZipPayload::writePlain(const std::string &dirName) {
  std::lock_guard<std::mutex> lock(_mtx);
  for (const auto &filePair : files) {
    fs::path fName(dirName);
    fName /= filePair.first;

    fs::create_directories(fName.parent_path());
    std::ofstream fStream(fName, std::ofstream::out);
    if (fStream.fail() || !fStream.good()) {
      llvm::errs() << "Unable to open output file " << fName << "\n";
      continue;
    }
    fStream << filePair.second;
    fStream.close();
  }
}

void ZipPayload::writePlain(llvm::raw_ostream &stream) {
  std::vector<fs::path> orderedNames = orderedFileNames();
  stream << "------------------------------------------\n";
  stream << "Plaintext payload: " << prefix << "\n";
  stream << "------------------------------------------\n";
  stream << "Manifest:\n";
  for (auto &fName : orderedNames)
    stream << fName << "\n";
  stream << "------------------------------------------\n";
  for (auto &fName : orderedNames) {
    stream << "File: " << fName << "\n";
    stream << files[fName];
    if (*(files[fName].rbegin()) != '\n')
      stream << "\n";
    stream << "------------------------------------------\n";
  }
}

void ZipPayload::writePlain(std::ostream &stream) {
  llvm::raw_os_ostream llstream(stream);
  writePlain(llstream);
}

namespace {
void setFilePermissions(zip_int64_t fileIndex, fs::path &fName,
                        zip_t *new_archive) {
  zip_uint8_t opsys;
  zip_uint32_t attributes;
  zip_file_get_external_attributes(new_archive, fileIndex, 0, &opsys,
                                   &attributes);
  if (opsys == ZIP_OPSYS_UNIX) {
    zip_uint32_t mask = UINT32_MAX; // all 1s for negative mask
    mask ^= (S_IWGRP << 16);        // turn off write for the group
    mask ^= (S_IWOTH << 16);        // turn off write for others

    // apply negative write mask
    attributes &= mask;

    // if executable turn on S_IXUSR
    if (fName.has_extension() && fName.extension() == ".sh")
      attributes |= (S_IXUSR << 16); // turn on execute for user

    // set new attributes
    zip_file_set_external_attributes(new_archive, fileIndex, 0, opsys,
                                     attributes);
  }
}
} // end anonymous namespace

void ZipPayload::writeZip(llvm::raw_ostream &stream) {
  llvm::outs() << "Writing zip to stream\n";
  // first add the manifest
  addManifest();

  // zip archive stuff
  zip_source_t *new_archive_src;
  zip_source_t *file_src;
  zip_t *new_archive;
  zip_error_t error;

  //===---- Initialize archive ----===//
  zip_error_init(&error);

  // open a zip source, buffer is allocated internally to libzip
  if ((new_archive_src = zip_source_buffer_create(nullptr, 0, 0, &error)) ==
      nullptr) {
    llvm::errs() << "Can't create zip source for new archive: "
                 << zip_error_strerror(&error) << "\n";
    zip_error_fini(&error);
    return;
  }

  // make sure the new source buffer stays around after closing the archive
  zip_source_keep(new_archive_src);

  // create and open an archive from the new archive source
  if ((new_archive = zip_open_from_source(new_archive_src, ZIP_TRUNCATE,
                                          &error)) == nullptr) {
    llvm::errs() << "Can't create/open an archive from the new archive source: "
                 << zip_error_strerror(&error) << "\n";
    zip_source_free(new_archive_src);
    zip_error_fini(&error);
    return;
  }
  zip_error_fini(&error);

  llvm::outs() << "Zip buffer created, adding files to archive\n";
  // archive is now allocated and created, need to fill it with files/data
  std::vector<fs::path> orderedNames = orderedFileNames();
  for (auto &fName : orderedNames) {
    llvm::outs() << "Adding file " << fName << " to archive buffer ("
                 << files[fName].size() << " bytes)\n";

    //===---- Add file ----===//
    // init the error object
    zip_error_init(&error);

    // first create a zip source from the file data
    if ((file_src = zip_source_buffer_create(files[fName].c_str(),
                                             files[fName].size(), 0, &error)) ==
        nullptr) {
      llvm::errs() << "Can't create zip source for " << fName << " : "
                   << zip_error_strerror(&error) << "\n";
      zip_error_fini(&error);
      continue;
    }
    zip_error_fini(&error);

    // now add it to the archive
    zip_int64_t fileIndex = -1;
    if ((fileIndex = zip_file_add(new_archive, fName.c_str(), file_src,
                                  ZIP_FL_OVERWRITE)) < 0) {
      llvm::errs() << "Problem adding file " << fName
                   << " to archive: " << zip_strerror(new_archive) << "\n";
      continue;
    }

    setFilePermissions(fileIndex, fName, new_archive);
  }

  //===---- Shutdown archive ----===//
  // shutdown the archive, write central directory
  if (zip_close(new_archive) < 0) {
    llvm::errs() << "Problem closing new zip archive: "
                 << zip_strerror(new_archive) << "\n";
    return;
  }

  //===---- Repen for copying ----===//
  // reopen the archive stored in the new_archive_src
  zip_source_open(new_archive_src);
  // seek to the end of the archive
  zip_source_seek(new_archive_src, 0, SEEK_END);
  // get the number of bytes
  zip_int64_t sz = zip_source_tell(new_archive_src);
  llvm::outs() << "Zip buffer is of size " << sz << " bytes\n";

  // allocate a new buffer to copy the archive into
  char *outbuffer = (char *)malloc(sz);
  if (!outbuffer) {
    llvm::errs()
        << "Unable to allocate output buffer for writing zip to stream\n";
    zip_source_close(new_archive_src);
    return;
  }

  // seek back to the begining of the archive
  zip_source_seek(new_archive_src, 0, SEEK_SET);
  // copy the entire archive into the output bufffer
  zip_source_read(new_archive_src, outbuffer, sz);
  // all done
  zip_source_close(new_archive_src);

  // output the new archive to the stream
  stream.write(outbuffer, sz);
  stream.flush();
  free(outbuffer);
}

void ZipPayload::writeZip(std::ostream &stream) {
  llvm::raw_os_ostream llstream(stream);
  writeZip(llstream);
}

void ZipPayload::write(llvm::raw_ostream &stream) { writeZip(stream); }

void ZipPayload::write(std::ostream &stream) { writeZip(stream); }
