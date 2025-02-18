// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fidl/coding.h>
#include <lib/fidl/internal.h>
#include <lib/fidl/visitor.h>
#include <lib/fidl/walker.h>
#include <lib/stdcompat/variant.h>
#include <stdalign.h>
#include <zircon/assert.h>
#include <zircon/compiler.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef __Fuchsia__
#include <zircon/syscalls.h>
#endif

// TODO(kulakowski) Design zx_status_t error values.

namespace {

template <typename Byte>
struct DecodingPosition {
  Byte* addr;
  DecodingPosition operator+(uint32_t size) const { return DecodingPosition{addr + size}; }
  DecodingPosition& operator+=(uint32_t size) {
    addr += size;
    return *this;
  }
  template <typename T, typename U = std::conditional_t<std::is_const<Byte>::value, const T, T>>
  constexpr U* Get() const {
    return reinterpret_cast<U*>(addr);
  }
};

struct EnvelopeCheckpoint {
  uint32_t num_bytes;
  uint32_t num_handles;
};

constexpr zx_rights_t subtract_rights(zx_rights_t minuend, zx_rights_t subtrahend) {
  return minuend & ~subtrahend;
}
static_assert(subtract_rights(0b011, 0b101) == 0b010, "ensure rights subtraction works correctly");

enum class Mode { Decode, Validate };

template <Mode mode, typename T, typename U>
void AssignInDecode(T* ptr, U value) {
  static_assert(mode == Mode::Decode, "only assign if decode");
  *ptr = value;
}

template <Mode mode, typename T, typename U>
void AssignInDecode(const T* ptr, U value) {
  static_assert(mode == Mode::Validate, "don't assign if validate");
  // nothing in validate mode
}

template <typename Byte>
using BaseVisitor =
    fidl::Visitor<std::conditional_t<std::is_const<Byte>::value, fidl::NonMutatingVisitorTrait,
                                     fidl::MutatingVisitorTrait>,
                  DecodingPosition<Byte>, EnvelopeCheckpoint>;

template <Mode mode, typename Byte>
class FidlDecoder final : public BaseVisitor<Byte> {
 public:
  FidlDecoder(Byte* bytes, uint32_t num_bytes, const zx_handle_t* handles, uint32_t num_handles,
              uint32_t next_out_of_line, const char** out_error_msg, bool skip_unknown_handles)
      : bytes_(bytes),
        num_bytes_(num_bytes),
        num_handles_(num_handles),
        next_out_of_line_(next_out_of_line),
        out_error_msg_(out_error_msg),
        skip_unknown_handles_(skip_unknown_handles) {
    if (likely(handles != nullptr)) {
      handles_ = handles;
    }
  }

  FidlDecoder(Byte* bytes, uint32_t num_bytes, const zx_handle_info_t* handle_infos,
              uint32_t num_handle_infos, uint32_t next_out_of_line, const char** out_error_msg,
              bool skip_unknown_handles)
      : bytes_(bytes),
        num_bytes_(num_bytes),
        num_handles_(num_handle_infos),
        next_out_of_line_(next_out_of_line),
        out_error_msg_(out_error_msg),
        skip_unknown_handles_(skip_unknown_handles) {
    if (likely(handle_infos != nullptr)) {
      handles_ = handle_infos;
    }
  }

  using Position = typename BaseVisitor<Byte>::Position;
  using Status = typename BaseVisitor<Byte>::Status;
  using PointeeType = typename BaseVisitor<Byte>::PointeeType;
  using ObjectPointerPointer = typename BaseVisitor<Byte>::ObjectPointerPointer;
  using HandlePointer = typename BaseVisitor<Byte>::HandlePointer;
  using CountPointer = typename BaseVisitor<Byte>::CountPointer;
  using EnvelopePointer = typename BaseVisitor<Byte>::EnvelopePointer;

  static constexpr bool kOnlyWalkResources = false;
  static constexpr bool kContinueAfterConstraintViolation = false;

  Status VisitAbsentPointerInNonNullableCollection(ObjectPointerPointer object_ptr_ptr) {
    SetError("absent pointer disallowed in non-nullable collection");
    return Status::kConstraintViolationError;
  }

  Status VisitPointer(Position ptr_position, PointeeType pointee_type,
                      ObjectPointerPointer object_ptr_ptr, uint32_t inline_size,
                      Position* out_position) {
    uint32_t new_offset;
    if (unlikely(!FidlAddOutOfLine(next_out_of_line_, inline_size, &new_offset))) {
      SetError("overflow updating out-of-line offset");
      return Status::kMemoryError;
    }
    if (unlikely(new_offset > num_bytes_)) {
      SetError("message tried to access more than provided number of bytes");
      return Status::kMemoryError;
    }
    {
      if (inline_size % FIDL_ALIGNMENT != 0) {
        // Validate the last 8-byte block.
        const uint64_t* block_end = reinterpret_cast<const uint64_t*>(&bytes_[new_offset]) - 1;
        uint64_t padding_len = new_offset - next_out_of_line_ - inline_size;
        uint64_t padding_mask = ~0ull << (64 - 8 * padding_len);
        auto status = ValidatePadding(block_end, padding_mask);
        if (status != Status::kSuccess) {
          return status;
        }
      }
    }
    if (unlikely(pointee_type == PointeeType::kString)) {
      auto status = fidl_validate_string(reinterpret_cast<const char*>(&bytes_[next_out_of_line_]),
                                         inline_size);
      if (status != ZX_OK) {
        SetError("encountered invalid UTF8 string");
        return Status::kConstraintViolationError;
      }
    }
    *out_position = Position{bytes_ + next_out_of_line_};
    AssignInDecode<mode>(
        object_ptr_ptr,
        reinterpret_cast<std::remove_pointer_t<ObjectPointerPointer>>(&bytes_[next_out_of_line_]));

    next_out_of_line_ = new_offset;
    return Status::kSuccess;
  }

  Status VisitHandleInfo(Position handle_position, HandlePointer handle,
                         zx_rights_t required_handle_rights,
                         zx_obj_type_t required_handle_subtype) {
// Disable this function for clang static analyzer because:
//   This function would never be called in Validate mode, however, the function is compiled with
//   Validate mode in compile time even though mode would never be Validate in run time.
//   The clang static analyzer could not associate the runtime mode with compile time 'mode', so it
//   would construct a code path with compile time mode as Validate and runtime mode as Decode,
//   causing 'AssignInDecode' function to be called with Validate mode and causing
//   'received_handle' to be leaked.
#ifndef __clang_analyzer__
    assert(mode == Mode::Decode);
    assert(has_handle_infos());
    zx_handle_info_t received_handle_info = handle_infos()[handle_idx_];
    zx_handle_t received_handle = received_handle_info.handle;
    if (unlikely(received_handle == ZX_HANDLE_INVALID)) {
      SetError("invalid handle detected in handle table");
      return Status::kConstraintViolationError;
    }

    const char* error = nullptr;
    zx_status_t status = FidlEnsureHandleRights(
        &received_handle, received_handle_info.type, received_handle_info.rights,
        required_handle_subtype, required_handle_rights, &error);
    if (unlikely(status != ZX_OK)) {
      SetError(error);
      return Status::kConstraintViolationError;
    }

    AssignInDecode<mode>(handle, received_handle);
    handle_idx_++;
    return Status::kSuccess;
#endif  //  #ifndef __clang_analyzer__
  }

  Status VisitHandle(Position handle_position, HandlePointer handle,
                     zx_rights_t required_handle_rights, zx_obj_type_t required_handle_subtype) {
    if (unlikely(*handle != FIDL_HANDLE_PRESENT)) {
      SetError("message tried to decode a garbage handle");
      return Status::kConstraintViolationError;
    }
    if (unlikely(handle_idx_ == num_handles_)) {
      SetError("message decoded too many handles");
      return Status::kConstraintViolationError;
    }

    if (mode == Mode::Validate) {
      handle_idx_++;
      return Status::kSuccess;
    }

    if (has_handles()) {
      if (unlikely(handles()[handle_idx_] == ZX_HANDLE_INVALID)) {
        SetError("invalid handle detected in handle table");
        return Status::kConstraintViolationError;
      }
      AssignInDecode<mode>(handle, handles()[handle_idx_]);
      handle_idx_++;
      return Status::kSuccess;
    } else if (likely(has_handle_infos())) {
      return VisitHandleInfo(handle_position, handle, required_handle_rights,
                             required_handle_subtype);
    } else {
      SetError("decoder noticed a handle is present but the handle table is empty");
      AssignInDecode<mode>(handle, ZX_HANDLE_INVALID);
      return Status::kConstraintViolationError;
    }
  }

  Status VisitVectorOrStringCount(CountPointer ptr) { return Status::kSuccess; }

  template <typename MaskType>
  Status VisitInternalPadding(Position padding_position, MaskType mask) {
    return ValidatePadding(padding_position.template Get<const MaskType>(), mask);
  }

  EnvelopeCheckpoint EnterEnvelope() {
    return {
        .num_bytes = next_out_of_line_,
        .num_handles = handle_idx_,
    };
  }

  Status LeaveEnvelope(EnvelopePointer envelope, EnvelopeCheckpoint prev_checkpoint) {
    // Now that the envelope has been consumed, check the correctness of the envelope header.
    uint32_t num_bytes = next_out_of_line_ - prev_checkpoint.num_bytes;
    uint32_t num_handles = handle_idx_ - prev_checkpoint.num_handles;
    if (unlikely(envelope->num_bytes != num_bytes)) {
      SetError("Envelope num_bytes was mis-sized");
      return Status::kConstraintViolationError;
    }
    if (unlikely(envelope->num_handles != num_handles)) {
      SetError("Envelope num_handles was mis-sized");
      return Status::kConstraintViolationError;
    }
    return Status::kSuccess;
  }

  Status VisitUnknownEnvelope(EnvelopePointer envelope, FidlIsResource is_resource) {
    if (mode == Mode::Validate) {
      handle_idx_ += envelope->num_handles;
      return Status::kSuccess;
    }

    // If we do not have the coding table for this payload,
    // treat it as unknown and close its contained handles
    if (unlikely(envelope->num_handles > 0)) {
      uint32_t total_unknown_handles;
      if (add_overflow(unknown_handle_idx_, envelope->num_handles, &total_unknown_handles)) {
        SetError("number of unknown handles overflows");
        return Status::kConstraintViolationError;
      }
      if (total_unknown_handles > ZX_CHANNEL_MAX_MSG_HANDLES) {
        SetError("number of unknown handles exceeds unknown handle array size");
        return Status::kConstraintViolationError;
      }
      // If skip_unknown_handles_ is true, leave the unknown handles intact
      // for something else to process (e.g. HLCPP Decode)
      if (skip_unknown_handles_ && is_resource == kFidlIsResource_Resource) {
        handle_idx_ += envelope->num_handles;
        return Status::kSuccess;
      }
      // Receiving unknown handles for a resource type is only an error if
      // skip_unknown_handles_ is true, i.e. the walker itself is not
      // automatically closing all unknown handles (making it impossible for
      // the domain object to store the unknown handles).
      if (unlikely(skip_unknown_handles_ && is_resource == kFidlIsResource_NotResource)) {
        SetError("received unknown handles for a non-resource type");
        return Status::kConstraintViolationError;
      }
      if (has_handles()) {
        memcpy(&unknown_handles_[unknown_handle_idx_], &handles()[handle_idx_],
               envelope->num_handles * sizeof(zx_handle_t));
        handle_idx_ += envelope->num_handles;
        unknown_handle_idx_ += envelope->num_handles;
      } else if (has_handle_infos()) {
        uint32_t end = handle_idx_ + envelope->num_handles;
        for (; handle_idx_ < end; handle_idx_++, unknown_handle_idx_++) {
          unknown_handles_[unknown_handle_idx_] = handle_infos()[handle_idx_].handle;
        }
      }
    }

    return Status::kSuccess;
  }

  void OnError(const char* error) { SetError(error); }

  zx_status_t status() const { return status_; }

  bool DidConsumeAllBytes() const { return next_out_of_line_ == num_bytes_; }

  bool DidConsumeAllHandles() const { return handle_idx_ == num_handles_; }

  uint32_t unknown_handle_idx() const { return unknown_handle_idx_; }

  const zx_handle_t* unknown_handles() const { return unknown_handles_; }

 private:
  void SetError(const char* error) {
    if (status_ != ZX_OK) {
      return;
    }
    status_ = ZX_ERR_INVALID_ARGS;
    if (!out_error_msg_) {
      return;
    }
    *out_error_msg_ = error;
  }

  template <typename MaskType>
  Status ValidatePadding(const MaskType* padding_ptr, MaskType mask) {
    if ((*padding_ptr & mask) != 0) {
      SetError("non-zero padding bytes detected");
      return Status::kConstraintViolationError;
    }
    return Status::kSuccess;
  }

  bool has_handles() const { return cpp17::holds_alternative<const zx_handle_t*>(handles_); }
  bool has_handle_infos() const {
    return cpp17::holds_alternative<const zx_handle_info_t*>(handles_);
  }
  const zx_handle_t* handles() const { return cpp17::get<const zx_handle_t*>(handles_); }
  const zx_handle_info_t* handle_infos() const {
    return cpp17::get<const zx_handle_info_t*>(handles_);
  }

  // Message state passed in to the constructor.
  Byte* const bytes_;
  const uint32_t num_bytes_;
  cpp17::variant<cpp17::monostate, const zx_handle_t*, const zx_handle_info_t*> handles_;
  const uint32_t num_handles_;
  uint32_t next_out_of_line_;
  const char** const out_error_msg_;
  // HLCPP first uses FidlDecoder to do an in-place decode, then extracts data
  // out into domain objects. Since HLCPP stores unknown handles
  // (and LLCPP does not), this field allows HLCPP to use the decoder while
  // keeping unknown handles in flexible resource unions intact.
  bool skip_unknown_handles_;

  // Decoder state
  zx_status_t status_ = ZX_OK;
  uint32_t handle_idx_ = 0;
  uint32_t unknown_handle_idx_ = 0;
  zx_handle_t unknown_handles_[ZX_CHANNEL_MAX_MSG_HANDLES];
};

template <typename HandleType, Mode mode>
zx_status_t fidl_decode_impl(const fidl_type_t* type, void* bytes, uint32_t num_bytes,
                             const HandleType* handles, uint32_t num_handles,
                             const char** out_error_msg,
                             void (*close_handles)(const HandleType*, uint32_t),
                             bool close_unknown_union_handles) {
  auto drop_all_handles = [&]() { close_handles(handles, num_handles); };
  auto set_error = [&out_error_msg](const char* msg) {
    if (out_error_msg)
      *out_error_msg = msg;
  };
  if (unlikely(handles == nullptr && num_handles != 0)) {
    set_error("Cannot provide non-zero handle count and null handle pointer");
    return ZX_ERR_INVALID_ARGS;
  }
  if (unlikely(bytes == nullptr)) {
    set_error("Cannot decode null bytes");
    drop_all_handles();
    return ZX_ERR_INVALID_ARGS;
  }
  if (unlikely(!FidlIsAligned(reinterpret_cast<uint8_t*>(bytes)))) {
    set_error("Bytes must be aligned to FIDL_ALIGNMENT");
    drop_all_handles();
    return ZX_ERR_INVALID_ARGS;
  }

  zx_status_t status;
  size_t primary_size;
  if (unlikely((status = fidl::PrimaryObjectSize(type, &primary_size, out_error_msg)) != ZX_OK)) {
    drop_all_handles();
    return status;
  }

  uint32_t next_out_of_line;
  if (unlikely((status = fidl::StartingOutOfLineOffset(type, num_bytes, &next_out_of_line,
                                                       out_error_msg)) != ZX_OK)) {
    drop_all_handles();
    return status;
  }

  uint8_t* b = reinterpret_cast<uint8_t*>(bytes);
  for (size_t i = primary_size; i < size_t(next_out_of_line); i++) {
    if (b[i] != 0) {
      set_error("non-zero padding bytes detected");
      drop_all_handles();
      return ZX_ERR_INVALID_ARGS;
    }
  }

  FidlDecoder<mode, uint8_t> decoder(b, num_bytes, handles, num_handles, next_out_of_line,
                                     out_error_msg, close_unknown_union_handles);
  fidl::Walk(decoder, type, DecodingPosition<uint8_t>{b});

  if (unlikely(decoder.status() != ZX_OK)) {
    drop_all_handles();
    return decoder.status();
  }
  if (unlikely(!decoder.DidConsumeAllBytes())) {
    set_error("message did not decode all provided bytes");
    drop_all_handles();
    return ZX_ERR_INVALID_ARGS;
  }
  if (unlikely(!decoder.DidConsumeAllHandles())) {
    set_error("message did not decode all provided handles");
    drop_all_handles();
    return ZX_ERR_INVALID_ARGS;
  }

  (void)FidlHandleCloseMany(decoder.unknown_handles(), decoder.unknown_handle_idx());
  return ZX_OK;
}

void close_handles_op(const zx_handle_t* handles, uint32_t max_idx) {
  // Return value intentionally ignored. This is best-effort cleanup.
  FidlHandleCloseMany(handles, max_idx);
}

void close_handle_infos_op(const zx_handle_info_t* handle_infos, uint32_t max_idx) {
  // Return value intentionally ignored. This is best-effort cleanup.
  FidlHandleInfoCloseMany(handle_infos, max_idx);
}

}  // namespace

zx_status_t fidl_decode_skip_unknown_handles(const fidl_type_t* type, void* bytes,
                                             uint32_t num_bytes, const zx_handle_t* handles,
                                             uint32_t num_handles, const char** out_error_msg) {
  return fidl_decode_impl<zx_handle_t, Mode::Decode>(type, bytes, num_bytes, handles, num_handles,
                                                     out_error_msg, close_handles_op, true);
}

zx_status_t fidl_decode(const fidl_type_t* type, void* bytes, uint32_t num_bytes,
                        const zx_handle_t* handles, uint32_t num_handles,
                        const char** out_error_msg) {
  return fidl_decode_impl<zx_handle_t, Mode::Decode>(type, bytes, num_bytes, handles, num_handles,
                                                     out_error_msg, close_handles_op, false);
}

zx_status_t fidl_decode_etc_skip_unknown_handles(const fidl_type_t* type, void* bytes,
                                                 uint32_t num_bytes,
                                                 const zx_handle_info_t* handle_infos,
                                                 uint32_t num_handle_infos,
                                                 const char** out_error_msg) {
  return fidl_decode_impl<zx_handle_info_t, Mode::Decode>(type, bytes, num_bytes, handle_infos,
                                                          num_handle_infos, out_error_msg,
                                                          close_handle_infos_op, true);
}

zx_status_t fidl_decode_etc(const fidl_type_t* type, void* bytes, uint32_t num_bytes,
                            const zx_handle_info_t* handle_infos, uint32_t num_handle_infos,
                            const char** out_error_msg) {
  return fidl_decode_impl<zx_handle_info_t, Mode::Decode>(type, bytes, num_bytes, handle_infos,
                                                          num_handle_infos, out_error_msg,
                                                          close_handle_infos_op, false);
}

zx_status_t fidl_decode_msg(const fidl_type_t* type, fidl_incoming_msg_t* msg,
                            const char** out_error_msg) {
  return fidl_decode_etc(type, msg->bytes, msg->num_bytes, msg->handles, msg->num_handles,
                         out_error_msg);
}

zx_status_t fidl_validate(const fidl_type_t* type, const void* bytes, uint32_t num_bytes,
                          uint32_t num_handles, const char** out_error_msg) {
  auto set_error = [&out_error_msg](const char* msg) {
    if (out_error_msg)
      *out_error_msg = msg;
  };
  if (bytes == nullptr) {
    set_error("Cannot validate null bytes");
    return ZX_ERR_INVALID_ARGS;
  }

  zx_status_t status;
  size_t primary_size;
  if (unlikely((status = fidl::PrimaryObjectSize(type, &primary_size, out_error_msg)) != ZX_OK)) {
    return status;
  }

  uint32_t next_out_of_line;
  if ((status = fidl::StartingOutOfLineOffset(type, num_bytes, &next_out_of_line, out_error_msg)) !=
      ZX_OK) {
    return status;
  }

  const uint8_t* b = reinterpret_cast<const uint8_t*>(bytes);
  for (size_t i = primary_size; i < size_t(next_out_of_line); i++) {
    if (b[i] != 0) {
      set_error("non-zero padding bytes detected");
      return ZX_ERR_INVALID_ARGS;
    }
  }

  FidlDecoder<Mode::Validate, const uint8_t> validator(
      b, num_bytes, (zx_handle_t*)(nullptr), num_handles, next_out_of_line, out_error_msg, false);
  fidl::Walk(validator, type, DecodingPosition<const uint8_t>{b});

  if (validator.status() == ZX_OK) {
    if (!validator.DidConsumeAllBytes()) {
      set_error("message did not consume all provided bytes");
      return ZX_ERR_INVALID_ARGS;
    }
    if (!validator.DidConsumeAllHandles()) {
      set_error("message did not reference all provided handles");
      return ZX_ERR_INVALID_ARGS;
    }
  }

  return validator.status();
}

zx_status_t fidl_validate_msg(const fidl_type_t* type, const fidl_outgoing_msg_byte_t* msg,
                              const char** out_error_msg) {
  return fidl_validate(type, msg->bytes, msg->num_bytes, msg->num_handles, out_error_msg);
}
