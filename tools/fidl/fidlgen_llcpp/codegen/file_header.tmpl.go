// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const fileHeaderTmpl = `
{{- define "Header" -}}
{{- UseWire -}}
// WARNING: This file is machine generated by fidlgen.

#pragma once

#include <algorithm>
#include <cstddef>
#include <variant>

#include <lib/fidl/internal.h>
#include <lib/fidl/llcpp/array.h>
#include <lib/fidl/llcpp/buffer_allocator.h>
#include <lib/fidl/llcpp/buffer_then_heap_allocator.h>
#include <lib/fidl/llcpp/coding.h>
#include <lib/fidl/llcpp/envelope.h>
#include <lib/fidl/llcpp/errors.h>
#include <lib/fidl/llcpp/memory.h>
#include <lib/fidl/llcpp/message.h>
#include <lib/fidl/llcpp/message_storage.h>
#include <lib/fidl/llcpp/object_view.h>
#include <lib/fidl/llcpp/string_view.h>
#include <lib/fidl/llcpp/tracking_ptr.h>
#include <lib/fidl/llcpp/traits.h>
#include <lib/fidl/llcpp/vector_view.h>
#include <lib/fit/function.h>
#include <lib/stdcompat/optional.h>
#ifdef __Fuchsia__
{{- PushNamespace }}
#include <lib/fidl/llcpp/client.h>
#include <lib/fidl/llcpp/client_end.h>
#include <lib/fidl/llcpp/connect_service.h>
#include <lib/fidl/llcpp/result.h>
#include <lib/fidl/llcpp/server.h>
#include <lib/fidl/llcpp/server_end.h>
#include <lib/fidl/llcpp/service_handler_interface.h>
#include <lib/fidl/llcpp/sync_call.h>
#include <lib/fidl/llcpp/transaction.h>
#include <lib/fidl/txn_header.h>
{{ range .HandleTypes -}}
#include <lib/zx/{{ . }}.h>
{{ end -}}
{{- PopNamespace }}
#endif  // __Fuchsia__
#include <zircon/fidl.h>
{{ if .Headers -}}
{{ "" }}
{{ $root := . -}}
{{ range .Headers -}}
#include <{{ . }}/{{ $root.IncludeStem }}.h>
{{ end -}}
{{ end -}}

{{- range .Decls }}
{{- if Eq .Kind Kinds.Bits }}{{ template "BitsForwardDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Enum }}{{ template "EnumForwardDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Protocol }}{{ template "ProtocolForwardDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Service }}{{ template "ServiceForwardDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Struct }}{{ template "StructForwardDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Table }}{{ template "TableForwardDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Union }}{{ template "UnionForwardDeclaration" . }}{{- end }}
{{- end }}

{{- /* Declare tables and unions first, since they store their members
    out-of-line and so they only need forward declarations.
    See fxbug.dev/7919 formore context. */}}
{{- range .Decls }}
{{- if Eq .Kind Kinds.Table }}{{ template "TableDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Union }}{{ template "UnionDeclaration" . }}{{- end }}
{{- end }}

{{- range .Decls }}
{{- if Eq .Kind Kinds.Const }}{{ template "ConstDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Protocol }}{{ template "ProtocolDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Service }}{{ template "ServiceDeclaration" . }}{{- end }}
{{- if Eq .Kind Kinds.Struct }}{{ template "StructDeclaration" . }}{{- end }}
{{- end }}
{{ "" }}

{{ EnsureNamespace "fidl" }}

{{- range .Decls }}
{{- if Eq .Kind Kinds.Bits }}{{ template "BitsTraits" . }}{{- end }}
{{- if Eq .Kind Kinds.Protocol }}{{ template "ProtocolTraits" . }}{{- end }}
{{- if Eq .Kind Kinds.Struct }}{{ template "StructTraits" . }}{{- end }}
{{- if Eq .Kind Kinds.Table }}{{ template "TableTraits" . }}{{- end }}
{{- if Eq .Kind Kinds.Union }}{{ template "UnionTraits" . }}{{- end }}
{{- if Eq .Kind Kinds.Enum }}{{ template "EnumTraits" . }}{{- end }}
{{- end }}

{{- range .Decls }}
{{ EnsureNamespace . }}

{{- if Eq .Kind Kinds.Protocol }}
{{ template "ClientDeclaration" . }}
{{ "" }}
{{ template "EventSenderDeclaration" . }}
{{ "" }}
{{- end }}
{{- end }}
{{ "" }}

{{ EndOfFile }}
{{ end }}
`
