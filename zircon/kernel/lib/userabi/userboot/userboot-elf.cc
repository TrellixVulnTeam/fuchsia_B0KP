// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "userboot-elf.h"

#include <elf.h>
#include <string.h>
#include <zircon/compiler.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <iterator>

#include <elfload/elfload.h>
#include <fbl/algorithm.h>

#include "bootfs.h"
#include "util.h"

#define INTERP_PREFIX "lib/"

static zx_vaddr_t load(const zx::debuglog& log, const char* what, const zx::vmar& vmar,
                       const zx::vmo& vmo, uintptr_t* interp_off, size_t* interp_len,
                       zx::vmar* segments_vmar, size_t* stack_size, bool return_entry) {
  elf_load_header_t header;
  uintptr_t phoff;
  zx_status_t status = elf_load_prepare(vmo.get(), NULL, 0, &header, &phoff);
  check(log, status, "elf_load_prepare failed");

  elf_phdr_t phdrs[header.e_phnum];
  status = elf_load_read_phdrs(vmo.get(), phdrs, phoff, header.e_phnum);
  check(log, status, "elf_load_read_phdrs failed");

  if (interp_off != NULL && elf_load_find_interp(phdrs, header.e_phnum, interp_off, interp_len))
    return 0;

  if (stack_size != NULL) {
    for (size_t i = 0; i < header.e_phnum; ++i) {
      if (phdrs[i].p_type == PT_GNU_STACK && phdrs[i].p_memsz > 0)
        *stack_size = phdrs[i].p_memsz;
    }
  }

  zx_vaddr_t base, entry;
  zx_handle_t* vmar_ptr = (segments_vmar) ? segments_vmar->reset_and_get_address() : NULL;
  status = elf_load_map_segments(vmar.get(), &header, phdrs, vmo.get(), vmar_ptr, &base, &entry);
  check(log, status, "elf_load_map_segments failed");

  printl(log, "userboot: loaded %s at %p, entry point %p\n", what, (void*)base, (void*)entry);
  return return_entry ? entry : base;
}

zx_vaddr_t elf_load_vdso(const zx::debuglog& log, const zx::vmar& vmar, const zx::vmo& vmo) {
  return load(log, "vDSO", vmar, vmo, NULL, NULL, NULL, NULL, false);
}

enum loader_bootstrap_handle_index {
  BOOTSTRAP_EXEC_VMO,
  BOOTSTRAP_LOGGER,
  BOOTSTRAP_PROC,
  BOOTSTRAP_ROOT_VMAR,
  BOOTSTRAP_SEGMENTS_VMAR,
  BOOTSTRAP_THREAD,
  BOOTSTRAP_LOADER_SVC,
  BOOTSTRAP_HANDLES
};

#define LOADER_BOOTSTRAP_ENVIRON "LD_DEBUG=1"
#define LOADER_BOOTSTRAP_ENVIRON_NUM 1

struct loader_bootstrap_message {
  zx_proc_args_t header;
  uint32_t handle_info[BOOTSTRAP_HANDLES];
  char env[sizeof(LOADER_BOOTSTRAP_ENVIRON)];
};

static void stuff_loader_bootstrap(const zx::debuglog& log, const zx::process& proc,
                                   const zx::vmar& root_vmar, const zx::thread& thread,
                                   const zx::channel& to_child, zx::vmar segments_vmar, zx::vmo vmo,
                                   zx::channel* loader_svc) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
  struct loader_bootstrap_message msg = {
      .header =
          {
              .protocol = ZX_PROCARGS_PROTOCOL,
              .version = ZX_PROCARGS_VERSION,
              .handle_info_off = offsetof(struct loader_bootstrap_message, handle_info),
              .environ_off = offsetof(struct loader_bootstrap_message, env),
              .environ_num = LOADER_BOOTSTRAP_ENVIRON_NUM,
          },
      .handle_info =
          {
              [BOOTSTRAP_EXEC_VMO] = PA_HND(PA_VMO_EXECUTABLE, 0),
              [BOOTSTRAP_LOGGER] = PA_HND(PA_FD, 0),
              [BOOTSTRAP_PROC] = PA_HND(PA_PROC_SELF, 0),
              [BOOTSTRAP_ROOT_VMAR] = PA_HND(PA_VMAR_ROOT, 0),
              [BOOTSTRAP_SEGMENTS_VMAR] = PA_HND(PA_VMAR_LOADED, 0),
              [BOOTSTRAP_THREAD] = PA_HND(PA_THREAD_SELF, 0),
              [BOOTSTRAP_LOADER_SVC] = PA_HND(PA_LDSVC_LOADER, 0),
          },
      .env = LOADER_BOOTSTRAP_ENVIRON,
  };
#pragma GCC diagnostic pop
  zx_handle_t handles[] = {
      [BOOTSTRAP_EXEC_VMO] = vmo.release(),
      [BOOTSTRAP_LOGGER] = ZX_HANDLE_INVALID,
      [BOOTSTRAP_PROC] = ZX_HANDLE_INVALID,
      [BOOTSTRAP_ROOT_VMAR] = ZX_HANDLE_INVALID,
      [BOOTSTRAP_SEGMENTS_VMAR] = segments_vmar.release(),
      [BOOTSTRAP_THREAD] = ZX_HANDLE_INVALID,
      [BOOTSTRAP_LOADER_SVC] = ZX_HANDLE_INVALID,
  };
  check(log, zx_handle_duplicate(log.get(), ZX_RIGHT_SAME_RIGHTS, &handles[BOOTSTRAP_LOGGER]),
        "zx_handle_duplicate failed");
  check(log, zx_handle_duplicate(proc.get(), ZX_RIGHT_SAME_RIGHTS, &handles[BOOTSTRAP_PROC]),
        "zx_handle_duplicate failed");
  check(log,
        zx_handle_duplicate(root_vmar.get(), ZX_RIGHT_SAME_RIGHTS, &handles[BOOTSTRAP_ROOT_VMAR]),
        "zx_handle_duplicate failed");
  check(log, zx_handle_duplicate(thread.get(), ZX_RIGHT_SAME_RIGHTS, &handles[BOOTSTRAP_THREAD]),
        "zx_handle_duplicate failed");
  check(log,
        zx_channel_create(0, loader_svc->reset_and_get_address(), &handles[BOOTSTRAP_LOADER_SVC]),
        "zx_channel_create failed");

  zx_status_t status = to_child.write(0, &msg, sizeof(msg), handles, std::size(handles));
  check(log, status, "zx_channel_write of loader bootstrap message failed");
}

zx_vaddr_t elf_load_bootfs(const zx::debuglog& log, const Bootfs& bootfs, const char* root_prefix,
                           const zx::process& proc, const zx::vmar& vmar, const zx::thread& thread,
                           const char* filename, const zx::channel& to_child, size_t* stack_size,
                           zx::channel* loader_svc) {
  zx::vmo vmo = bootfs.Open(root_prefix, filename, "program");

  uintptr_t interp_off = 0;
  size_t interp_len = 0;
  zx_vaddr_t entry =
      load(log, filename, vmar, vmo, &interp_off, &interp_len, NULL, stack_size, true);
  if (interp_len > 0) {
    // While PT_INTERP names can be arbitrarily large, bootfs entries
    // have names of bounded length.
    constexpr size_t kInterpMaxLen = ZBI_BOOTFS_MAX_NAME_LEN;
    constexpr size_t kInterpPrefixLen = sizeof(INTERP_PREFIX) - 1;
    static_assert(kInterpMaxLen >= kInterpPrefixLen);
    constexpr size_t kInterpSuffixLen = kInterpMaxLen - kInterpPrefixLen;

    if (interp_len > kInterpSuffixLen) {
      return ZX_ERR_INVALID_ARGS;
    }

    // Add one for the trailing nul.
    char interp[kInterpMaxLen + 1];

    // Copy the prefix.
    memcpy(interp, INTERP_PREFIX, kInterpPrefixLen);

    // Copy the suffix.
    zx_status_t status = vmo.read(&interp[kInterpPrefixLen], interp_off, interp_len);
    if (status != ZX_OK)
      fail(log, "zx_vmo_read failed: %d", status);

    // Copy the nul.
    interp[kInterpPrefixLen + interp_len] = '\0';

    printl(log, "'%s' has PT_INTERP \"%s\"", filename, interp);

    zx::vmo interp_vmo = bootfs.Open(root_prefix, interp, "dynamic linker");
    zx::vmar interp_vmar;
    entry = load(log, interp, vmar, interp_vmo, NULL, NULL, &interp_vmar, NULL, true);

    stuff_loader_bootstrap(log, proc, vmar, thread, to_child, std::move(interp_vmar),
                           std::move(vmo), loader_svc);
  }
  return entry;
}
