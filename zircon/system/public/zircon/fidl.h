// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SYSROOT_ZIRCON_FIDL_H_
#define SYSROOT_ZIRCON_FIDL_H_

#include <assert.h>    // NOLINT(modernize-deprecated-headers, foobar)
#include <stdalign.h>  // NOLINT(modernize-deprecated-headers)
#include <stdint.h>    // NOLINT(modernize-*)
#include <zircon/compiler.h>
#include <zircon/types.h>

__BEGIN_CDECLS

// Fidl data types have a representation in a wire format. This wire
// format is shared by all language bindings, including C11 and C++.
//
// The C bindings also define a representation of fidl data types. For
// a given type, the size and alignment of all parts of the type agree
// with the wire format's representation. The C representation differs
// in the representation of pointers to out-of-line allocations. On
// the wire, allocations are encoded as either present or not. In C,
// they are actual pointers. The C representation also places any
// transferred handle types (including requests) inline. The wire
// format tracks handles separately, just like the underlying channel
// transport does.
//
// Turning the wire format into the C format is called decoding.
//
// Turning the C format into the wire format is called encoding.
//
// The formats are designed to allow for in-place coding, assuming all
// out-of-line allocations placed are in traversal order (defined
// below) with natural alignment.

// Bounds.

// Various fidl types, such as strings and vectors, may be bounded. If
// no explicit bound is given, then FIDL_MAX_SIZE is implied.

#define FIDL_MAX_SIZE UINT32_MAX

// Out of line allocations.

// The fidl wire format represents potential out-of-line allocations
// (corresponding to actual pointer types in the C format) as
// uintptr_t. For allocations that are actually present and that will
// be patched up with pointers during decoding, the FIDL_ALLOC_PRESENT
// value is used. For non-present nullable allocations, the
// FIDL_ALLOC_ABSENT value is used.

#define FIDL_ALLOC_PRESENT ((uintptr_t)UINTPTR_MAX)
#define FIDL_ALLOC_ABSENT ((uintptr_t)0)

// Out of line allocations are all 8 byte aligned.
// TODO(fxbug.dev/42792): Remove either this FIDL_ALIGN macro or the FidlAlign function in
// fidl/internal.h.
#define FIDL_ALIGNMENT ((size_t)8)
#define FIDL_ALIGN(a) (((a) + 7u) & ~7u)
#define FIDL_ALIGNDECL alignas(FIDL_ALIGNMENT)

// The maximum depth of out-of-line objects in the wire format.
// 0 is the initial depth, 1 is the first out of line object, etc.
// Tables count as two depth levels because the vector body and the
// table elements are both out of line.
#define FIDL_MAX_DEPTH 32

// An opaque struct containing metadata for encoding a particular fidl
// type. The actual length of the struct is different depending on the
// kind of fidl type it is describing.
typedef struct fidl_type fidl_type_t;

// Primitive types.

// Both on the wire and once deserialized, primitive fidl types
// correspond directly to C types. There is no intermediate layer of
// typedefs. For instance, fidl's float64 is generated as double.

// All primitive types are non-nullable.

// All primitive types are naturally sized and aligned on the wire.

// fidl     C         Meaning.
// ---------------------------------------------
// bool     bool      A boolean.
// int8     int8_t    An 8 bit signed integer.
// int16    int16_t   A 16 bit signed integer.
// int32    int32_t   A 32 bit signed integer.
// int64    int64_t   A 64 bit signed integer.
// uint8    uint8_t   An 8 bit unsigned integer.
// uint16   uint16_t  A 16 bit unsigned integer.
// uint32   uint32_t  A 32 bit unsigned integer.
// uint64   uint64_t  A 64 bit unsigned integer.
// float32  float     A 32 bit IEEE-754 float.
// float64  double    A 64 bit IEEE-754 float.

// Enums.

// Fidl enums have an undering integer type (one of int8, int16,
// int32, int64, uint8, uint16, uint32, or uint64). The wire format of
// an enum and the C format of an enum are the same as the
// corresponding primitive type.

// String types.

// Fidl strings are variable-length UTF-8 strings. Strings can be
// nullable (string?) or nonnullable (string); if nullable, the null
// string is distinct from the empty string. Strings can be bounded to
// a fixed byte length (e.g. string:40? is a nullable string of at
// most 40 bytes).

// Strings are not guaranteed to be nul terminated. Strings can
// contain embedded nuls throughout their length.

// The fidl wire format dictates that strings are valid UTF-8. It is
// up to clients to provide well-formed UTF-8 and servers to check for
// it. Message encoding and decoding can, but does not by default,
// perform this check.

// All deserialized string types are represented by the fidl_string_t
// structure. This structure consists of a size (in bytes) and a
// pointer to an out-of-line allocation of uint8_t, guaranteed to be
// at least as long as the length.

// The bound on a string type is not present in the serialized format,
// but is checked as part of validation.

typedef struct fidl_string {
  // Number of UTF-8 code units (bytes), must be 0 if |data| is null.
  uint64_t size;

  // Pointer to UTF-8 code units (bytes) or null
  char* data;
} fidl_string_t;

// When encoded, an absent nullable string is represented as a
// fidl_string_t with size 0 and FIDL_ALLOC_ABSENT data, with no
// out-of-line allocation associated with it. A present string
// (nullable or not) is represented as a fidl_string_t with some size
// and with data equal to FIDL_ALLOC_PRESENT, which the decoding
// process replaces with an actual pointer to the next out-of-line
// allocation.

// All string types:

// fidl       C              Meaning
// -----------------------------------------------------------------
// string     fidl_string_t  A string of arbitrary length.
// string?    fidl_string_t  An optional string of arbitrary length.
// string:N   fidl_string_t  A string up to N bytes long.
// string:N?  fidl_string_t  An optional string up to N bytes long.

// Arrays.

// On the wire, an array of N objects of type T (array<T, N>) is
// represented the same as N contiguous Ts. Equivalently, it is
// represented the same as a nonnullable struct containing N fields
// all of type T.

// In C, this is just represented as a C array of the corresponding C
// type.

// Vector types.

// Fidl vectors are variable-length arrays of a given type T. Vectors
// can be nullable (vector<T>?) or nonnullable (vector<T>); if
// nullable, the null vector is distinct from the empty
// vector. Vectors can be bounded to a fixed element length
// (e.g. vector<T>:40? is a nullable vector of at most 40 Ts).

// All deserialized vector types are represented by the fidl_vector_t
// structure. This structure consists of a count and a pointer to the
// bytes.

// The bound on a vector type is not present in the serialized format,
// but is checked as part of validation.

typedef struct fidl_vector {
  // Number of elements, must be 0 if |data| is null.
  uint64_t count;

  // Pointer to element data or null.
  void* data;
} fidl_vector_t;

// When encoded, an absent nullable vector is represented as a
// fidl_vector_t with size 0 and FIDL_ALLOC_ABSENT data, with no
// out-of-line allocation associated with it. A present vector
// (nullable or not) is represented as a fidl_vector_t with some size
// and with data equal to FIDL_ALLOC_PRESENT, which the decoding
// process replaces with an actual pointer to the next out-of-line
// allocation.

// All vector types:

// fidl          C              Meaning
// --------------------------------------------------------------------------
// vector<T>     fidl_vector_t  A vector of T, of arbitrary length.
// vector<T>?    fidl_vector_t  An optional vector of T, of arbitrary length.
// vector<T>:N   fidl_vector_t  A vector of T, up to N elements.
// vector<T>:N?  fidl_vector_t  An optional vector of T,  up to N elements.

// Envelope.

// An efficient way to encapsulate uninterpreted FIDL messages.
// - Stores a variable size uninterpreted payload out-of-line.
// - Payload may contain an arbitrary number of bytes and handles.
// - Allows for encapsulation of one FIDL message inside of another.
// - Building block for extensible structures such as tables & extensible
//   unions.

// When encoded for transfer, |data| indicates presence of content:
// - FIDL_ALLOC_ABSENT : envelope is null
// - FIDL_ALLOC_PRESENT : envelope is non-null, |data| is the next out-of-line object
// When decoded for consumption, |data| is a pointer to content.
// - nullptr : envelope is null
// - <valid pointer> : envelope is non-null, |data| is at indicated memory address

typedef struct {
  // The size of the entire envelope contents, including any additional
  // out-of-line objects that the envelope may contain. For example, a
  // vector<string>'s num_bytes for ["hello", "world"] would include the
  // string contents in the size, not just the outer vector. Always a multiple
  // of 8; must be zero if envelope is null.
  uint32_t num_bytes;

  // The number of handles in the envelope, including any additional
  // out-of-line objects that the envelope contains. Must be zero if envelope is null.
  uint32_t num_handles;

  // A pointer to the out-of-line envelope data in decoded form, or
  // FIDL_ALLOC_(ABSENT|PRESENT) in encoded form.
  union {
    void* data;
    uintptr_t presence;
  };
} fidl_envelope_t;

// Handle types.

// Handle types are encoded directly. Just like primitive types, there
// is no fidl-specific handle type. Generated fidl structures simply
// mention zx_handle_t.

// Handle types are either nullable (handle?), or not (handle); and
// either explicitly typed (e.g. handle<Channel> or handle<Job>), or
// not.

// All fidl handle types, regardless of subtype, are represented as
// zx_handle_t. The encoding tables do know the handle subtypes,
// however, for clients which wish to perform explicit checking.

// The following are the possible handle subtypes.

// process
// thread
// vmo
// channel
// event
// port
// interrupt
// iomap
// pci
// log
// socket
// resource
// eventpair
// job
// vmar
// fifo
// hypervisor
// guest
// timer

// All handle types are 4 byte sized and aligned on the wire.

// When encoded, absent nullable handles are represented as
// FIDL_HANDLE_ABSENT. Present handles, whether nullable or not, are
// represented as FIDL_HANDLE_PRESENT, which the decoding process will
// overwrite with the next handle value in the channel message.

#define FIDL_HANDLE_ABSENT ((zx_handle_t)ZX_HANDLE_INVALID)
#define FIDL_HANDLE_PRESENT ((zx_handle_t)UINT32_MAX)

// fidl        C            Meaning
// ------------------------------------------------------------------
// handle      zx_handle_t  Any valid handle.
// handle?     zx_handle_t  Any valid handle, or ZX_HANDLE_INVALID.
// handle<T>   zx_handle_t  Any valid T handle.
// handle<T>?  zx_handle_t  Any valid T handle, or ZX_HANDLE_INVALID.

// Unions.

// Fidl unions are a tagged sum type. The tag is a 4 bytes. For every
// union type, the fidl compiler generates an enum representing the
// different variants of the enum. This is followed, in C and on the
// wire, by large enough and aligned enough storage for all members of
// the union.

// Unions may be nullable. Nullable unions are represented as a
// pointer to an out of line allocation of tag-and-member. As with
// other out-of-line allocations, ones present on the wire take the
// value FIDL_ALLOC_PRESENT and those that are not are represented by
// FIDL_ALLOC_NULL. Nonnullable unions are represented inline as a
// tag-and-member.

// For each fidl union type, a corresponding C type is generated. They
// are all structs consisting of a fidl_union_tag_t discriminant,
// followed by an anonymous union of all the union members.

typedef uint32_t fidl_union_tag_t;

// fidl                 C                            Meaning
// --------------------------------------------------------------------
// union foo {...}      struct union_foo {           An inline union.
//                          fidl_union_tag_t tag;
//                          union {...};
//                      }
//
// union foo {...}?     struct union_foo*            A pointer to a
//                                                   union_foo, or else
//                                                   FIDL_ALLOC_ABSENT.

// Tables.

// Tables are 'flexible structs', where all members are optional, and new
// members can be added, or old members removed while preserving ABI
// compatibility. Each table member is referenced by ordinal, sequentially
// assigned from 1 onward, with no gaps. Each member content is stored
// out-of-line in an envelope, and a table is simply a vector of these envelopes
// with the requirement that the last envelope must be present in order
// to guarantee a canonical representation.

typedef struct {
  fidl_vector_t envelopes;
} fidl_table_t;

// Extensible unions.

// Extensible unions, or "xunions" (colloquially pronounced "zoo-nions") are
// similar to unions, except that storage for union members are out-of-line
// rather than inline. This enables union members to be added and removed while
// preserving ABI compatibility with the existing xunion definition.

typedef uint64_t fidl_xunion_tag_t;

enum {
  kFidlXUnionEmptyTag = 0,  // The tag representing an empty xunion.
};

typedef struct {
  fidl_xunion_tag_t tag;
  fidl_envelope_t envelope;
} fidl_xunion_t;

// Messages.

// All fidl messages share a common 16 byte header.

enum {
  kFidlWireFormatMagicNumberInitial = 1,
};

typedef struct fidl_message_header {
  zx_txid_t txid;
  uint8_t flags[3];
  // This value indicates the message's wire format. Two sides with different
  // wire formats are incompatible with each other
  uint8_t magic_number;
  uint64_t ordinal;
} fidl_message_header_t;

// Messages which do not have a response use zero as a special
// transaction id.

#define FIDL_TXID_NO_RESPONSE 0ul

// fidl_iovec_substition_t represents a pointer-width value substitution.
// The operation *ptr = value can be performed to overwrite the current value
// at a location with the original value.
typedef struct fidl_iovec_substitution {
  void** ptr;
  void* value;
} fidl_iovec_substitution_t;

// An outgoing FIDL message represented with contiguous bytes.
//
// See fidl_outgoing_msg_iovec_t for a represention using iovec.
typedef struct fidl_outgoing_msg_byte {
  // The bytes of the message.
  //
  // The bytes of the message might be in the encoded or decoded form.
  // Functions that take a |fidl_outgoing_msg_t| as an argument should document whether
  // the expect encoded or decoded messages.
  //
  // See |num_bytes| for the number of bytes in the message.
  void* bytes;

  // The handles of the message.
  //
  // See |num_handles| for the number of handles in the message.
  zx_handle_disposition_t* handles;

  // The number of bytes in |bytes|.
  uint32_t num_bytes;

  // The number of handles in |handles|.
  uint32_t num_handles;
} fidl_outgoing_msg_byte_t;

// An outgoing FIDL message represented with iovec.
//
// See fidl_outgoing_msg_byte_t for a represention using bytes.
typedef struct fidl_outgoing_msg_iovec {
  // The output iovecs of the message.
  //
  // See |num_iovecs| for the number of iovecs in the message.
  zx_channel_iovec_t* iovecs;

  // The total number of iovecs in |iovecs|.
  uint32_t num_iovecs;

  // The output handles of the message.
  //
  // See |num_handles| for the number of handles in the message.
  zx_handle_disposition_t* handles;

  // The number of handles in |handles|.
  uint32_t num_handles;
} fidl_outgoing_msg_iovec_t;

typedef uint8_t fidl_outgoing_msg_type;

#define FIDL_OUTGOING_MSG_TYPE_BYTE ((fidl_outgoing_msg_type)1)
#define FIDL_OUTGOING_MSG_TYPE_IOVEC ((fidl_outgoing_msg_type)2)

// An outgoing FIDL message, in either byte or iovec form.
typedef struct fidl_outgoing_msg {
  // Type of the outgoing message.
  fidl_outgoing_msg_type type;

  // Selection of the outgoing message body.
  union {
    fidl_outgoing_msg_byte_t byte;
    fidl_outgoing_msg_iovec_t iovec;
  };
} fidl_outgoing_msg_t;

// An incoming FIDL message.
typedef struct fidl_incoming_msg {
  // The bytes of the message.
  //
  // The bytes of the message might be in the encoded or decoded form.
  // Functions that take a |fidl_incoming_msg_t| as an argument should document whether
  // the expect encoded or decoded messages.
  //
  // See |num_bytes| for the number of bytes in the message.
  void* bytes;

  // The handles of the message, along with rights and type information.
  //
  // See |num_handles| for the number of handles in the message.
  zx_handle_info_t* handles;

  // The number of bytes in |bytes|.
  uint32_t num_bytes;

  // The number of handles in |handles|.
  uint32_t num_handles;
} fidl_incoming_msg_t;

// An outstanding FIDL transaction.
typedef struct fidl_txn fidl_txn_t;
struct fidl_txn {
  // Replies to the outstanding request and complete the FIDL transaction.
  //
  // Pass the |fidl_txn_t| object itself as the first parameter. The |msg|
  // should already be encoded. This function always consumes any handles
  // present in |msg|.
  //
  // Call |reply| only once for each |txn| object. After |reply| returns, the
  // |txn| object is considered invalid and might have been freed or reused
  // for another purpose.
  zx_status_t (*reply)(fidl_txn_t* txn, const fidl_outgoing_msg_t* msg);
};

// An epitaph is a message that a server sends just prior to closing the
// connection.  It provides an indication of why the connection is being closed.
// Epitaphs are defined in the FIDL wire format specification.  Once sent down
// the wire, the channel should be closed.
typedef struct fidl_epitaph {
  FIDL_ALIGNDECL

  // The method ordinal for all epitaphs must be kFidlOrdinalEpitaph
  fidl_message_header_t hdr;

  // The error associated with this epitaph is stored as a struct{int32} in
  // the message payload. System errors must be constants of type zx_status_t,
  // which are all negative. Positive numbers should be used for application
  // errors. A value of ZX_OK indicates no error.
  zx_status_t error;
} fidl_epitaph_t;

// This ordinal value is reserved for Epitaphs.
enum {
  kFidlOrdinalEpitaph = 0xFFFFFFFFFFFFFFFF,
};

// Assumptions.

// Ensure that FIDL_ALIGNMENT is sufficient.
static_assert(alignof(bool) <= FIDL_ALIGNMENT, "");
static_assert(alignof(int8_t) <= FIDL_ALIGNMENT, "");
static_assert(alignof(int16_t) <= FIDL_ALIGNMENT, "");
static_assert(alignof(int32_t) <= FIDL_ALIGNMENT, "");
static_assert(alignof(int64_t) <= FIDL_ALIGNMENT, "");
static_assert(alignof(uint8_t) <= FIDL_ALIGNMENT, "");
static_assert(alignof(uint16_t) <= FIDL_ALIGNMENT, "");
static_assert(alignof(uint32_t) <= FIDL_ALIGNMENT, "");
static_assert(alignof(uint64_t) <= FIDL_ALIGNMENT, "");
static_assert(alignof(float) <= FIDL_ALIGNMENT, "");
static_assert(alignof(double) <= FIDL_ALIGNMENT, "");
static_assert(alignof(void*) <= FIDL_ALIGNMENT, "");
static_assert(alignof(fidl_union_tag_t) <= FIDL_ALIGNMENT, "");
static_assert(alignof(fidl_message_header_t) <= FIDL_ALIGNMENT, "");

__END_CDECLS

#endif  // SYSROOT_ZIRCON_FIDL_H_
