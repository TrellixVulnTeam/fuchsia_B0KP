// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package golang

import (
	"bytes"
	"fmt"
	"go/format"
	"io"
	"strconv"
	"strings"
	"text/template"

	gidlir "go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	gidlmixer "go.fuchsia.dev/fuchsia/tools/fidl/gidl/mixer"
	fidl "go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

// withGoFmt wraps a template that produces Go source code, and formats the
// execution result using go/format.
type withGoFmt struct {
	template *template.Template
}

func (w withGoFmt) Execute(wr io.Writer, data interface{}) error {
	var b bytes.Buffer
	if err := w.template.Execute(&b, data); err != nil {
		return err
	}
	formatted, err := format.Source(b.Bytes())
	if err != nil {
		return err
	}
	_, err = wr.Write(formatted)
	return err
}

func buildBytes(bytes []byte) string {
	var builder strings.Builder
	builder.WriteString("[]byte{\n")
	for i, b := range bytes {
		builder.WriteString(fmt.Sprintf("0x%02x,", b))
		if i%8 == 7 {
			builder.WriteString("\n")
		}
	}
	builder.WriteString("}")
	return builder.String()
}

func buildHandleDefs(defs []gidlir.HandleDef) string {
	if len(defs) == 0 {
		return ""
	}
	var builder strings.Builder
	builder.WriteString("[]zx.ObjectType{\n")
	for i, d := range defs {
		switch d.Subtype {
		case fidl.Channel:
			builder.WriteString("zx.ObjectTypeChannel,")
		case fidl.Event:
			builder.WriteString("zx.ObjectTypeEvent,")
		default:
			panic(fmt.Sprintf("unsupported handle subtype: %s", d.Subtype))
		}
		// Write indices corresponding to the .gidl file handle_defs block.
		builder.WriteString(fmt.Sprintf(" // #%d\n", i))
	}
	builder.WriteString("}")
	return builder.String()
}

func buildHandleInfos(handles []gidlir.Handle) string {
	if len(handles) == 0 {
		return "nil"
	}
	var builder strings.Builder
	builder.WriteString("[]zx.HandleInfo{\n")
	for _, handle := range handles {
		builder.WriteString(fmt.Sprintf("{Handle: handles[%d], Type: handleTypes[%d]},\n", handle, handle))
	}
	builder.WriteString("}")
	return builder.String()
}

func buildUnknownData(data gidlir.UnknownData) string {
	return fmt.Sprintf(
		"fidl.UnknownData{\nBytes: %s, \nHandles: %s,\n}",
		buildBytes(data.Bytes),
		buildHandleInfos(data.Handles))
}

func buildUnknownDataMap(fields []gidlir.Field) string {
	if len(fields) == 0 {
		return "nil"
	}
	var builder strings.Builder
	builder.WriteString("map[uint64]fidl.UnknownData{\n")
	for _, field := range fields {
		builder.WriteString(fmt.Sprintf(
			"%d: %s,",
			field.Key.UnknownOrdinal,
			buildUnknownData(field.Value.(gidlir.UnknownData))))
	}
	builder.WriteString("}")
	return builder.String()
}

func visit(value interface{}, decl gidlmixer.Declaration) string {
	switch value := value.(type) {
	case bool, int64, uint64, float64:
		switch decl := decl.(type) {
		case gidlmixer.PrimitiveDeclaration:
			return fmt.Sprintf("%#v", value)
		case *gidlmixer.BitsDecl, *gidlmixer.EnumDecl:
			return fmt.Sprintf("%s(%d)", typeLiteral(decl), value)
		}
	case gidlir.RawFloat:
		switch decl.(*gidlmixer.FloatDecl).Subtype() {
		case fidl.Float32:
			return fmt.Sprintf("math.Float32frombits(%#b)", value)
		case fidl.Float64:
			return fmt.Sprintf("math.Float64frombits(%#b)", value)
		}
	case string:
		if decl.IsNullable() {
			// Taking an address of a string literal is not allowed, so instead
			// we create a slice and get the address of its first element.
			return fmt.Sprintf("&[]string{%q}[0]", value)
		}
		return strconv.Quote(value)
	case gidlir.Handle:
		rawHandle := fmt.Sprintf("handles[%d]", value)
		handleDecl := decl.(*gidlmixer.HandleDecl)
		switch handleDecl.Subtype() {
		case fidl.Handle:
			return rawHandle
		case fidl.Channel:
			return fmt.Sprintf("zx.Channel(%s)", rawHandle)
		case fidl.Event:
			return fmt.Sprintf("zx.Event(%s)", rawHandle)
		default:
			panic(fmt.Sprintf("Handle subtype not supported %s", handleDecl.Subtype()))
		}
	case gidlir.Record:
		if decl, ok := decl.(gidlmixer.RecordDeclaration); ok {
			return onRecord(value, decl)
		}
	case []interface{}:
		if decl, ok := decl.(gidlmixer.ListDeclaration); ok {
			return onList(value, decl)
		}
	case nil:
		if !decl.IsNullable() {
			panic(fmt.Sprintf("got nil for non-nullable type: %T", decl))
		}
		_, ok := decl.(*gidlmixer.HandleDecl)
		if ok {
			return "zx.HandleInvalid"
		}
		return "nil"
	}
	panic(fmt.Sprintf("not implemented: %T", value))
}

func onRecord(value gidlir.Record, decl gidlmixer.RecordDeclaration) string {
	var fields []string
	if decl, ok := decl.(*gidlmixer.UnionDecl); ok && len(value.Fields) >= 1 {
		field := value.Fields[0]
		fullName := declName(decl)
		var tagValue string
		if field.Key.IsUnknown() {
			tagValue = fmt.Sprintf("%d", field.Key.UnknownOrdinal)
		} else {
			fieldName := fidl.ToUpperCamelCase(field.Key.Name)
			tagValue = fmt.Sprintf("%s%s", fullName, fieldName)
		}
		parts := strings.Split(string(decl.Name()), "/")
		unqualifiedName := fidl.ToLowerCamelCase(parts[len(parts)-1])
		fields = append(fields,
			fmt.Sprintf("I_%sTag: %s", unqualifiedName, tagValue))
	}
	_, isTable := decl.(*gidlmixer.TableDecl)
	var unknownTableFields []gidlir.Field
	for _, field := range value.Fields {
		if field.Key.IsUnknown() {
			if isTable {
				unknownTableFields = append(unknownTableFields, field)
			} else {
				fields = append(fields,
					fmt.Sprintf("I_unknownData: %s", buildUnknownData(field.Value.(gidlir.UnknownData))))
			}
			continue
		}
		fieldName := fidl.ToUpperCamelCase(field.Key.Name)
		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		fields = append(fields,
			fmt.Sprintf("%s: %s", fieldName, visit(field.Value, fieldDecl)))
		if isTable && field.Value != nil {
			fields = append(fields, fmt.Sprintf("%sPresent: true", fieldName))
		}
	}
	if len(unknownTableFields) > 0 {
		fields = append(fields,
			fmt.Sprintf("I_unknownData: %s", buildUnknownDataMap(unknownTableFields)))
	}

	if len(fields) == 0 {
		return fmt.Sprintf("%s{}", typeLiteral(decl))
	}
	// Insert newlines so that gofmt can produce good results.
	return fmt.Sprintf("%s{\n%s,\n}", typeLiteral(decl), strings.Join(fields, ",\n"))
}

func onList(value []interface{}, decl gidlmixer.ListDeclaration) string {
	elemDecl := decl.Elem()
	var elements []string
	for _, item := range value {
		elements = append(elements, visit(item, elemDecl))
	}
	if len(elements) == 0 {
		return fmt.Sprintf("%s{}", typeLiteral(decl))
	}
	// Insert newlines so that gofmt can produce good results.
	return fmt.Sprintf("%s{\n%s,\n}", typeLiteral(decl), strings.Join(elements, ",\n"))
}

func typeName(decl gidlmixer.Declaration) string {
	return typeNameHelper(decl, "*")
}

func typeLiteral(decl gidlmixer.Declaration) string {
	return typeNameHelper(decl, "&")
}

func typeNameHelper(decl gidlmixer.Declaration, pointerPrefix string) string {
	if !decl.IsNullable() {
		pointerPrefix = ""
	}

	switch decl := decl.(type) {
	case gidlmixer.PrimitiveDeclaration:
		return string(decl.Subtype())
	case gidlmixer.NamedDeclaration:
		return pointerPrefix + declName(decl)
	case *gidlmixer.StringDecl:
		return pointerPrefix + "string"
	case *gidlmixer.ArrayDecl:
		return fmt.Sprintf("[%d]%s", decl.Size(), typeName(decl.Elem()))
	case *gidlmixer.VectorDecl:
		return fmt.Sprintf("%s[]%s", pointerPrefix, typeName(decl.Elem()))
	case *gidlmixer.HandleDecl:
		switch decl.Subtype() {
		case fidl.Handle:
			return "zx.Handle"
		case fidl.Channel:
			return "zx.Channel"
		case fidl.Event:
			return "zx.Event"
		default:
			panic(fmt.Sprintf("Handle subtype not supported %s", decl.Subtype()))
		}
	default:
		panic(fmt.Sprintf("unhandled case %T", decl))
	}
}

func declName(decl gidlmixer.NamedDeclaration) string {
	return identifierName(decl.Name())
}

// TODO(fxbug.dev/39407): Such utilities (and their accompanying tests) would be
// useful as part of fidl or fidl to do FIDL-to-<target_lang>
// conversion.
func identifierName(qualifiedName string) string {
	parts := strings.Split(qualifiedName, "/")
	lastPartsIndex := len(parts) - 1
	for i, part := range parts {
		if i == lastPartsIndex {
			parts[i] = fidl.ToUpperCamelCase(part)
		} else {
			parts[i] = fidl.ToSnakeCase(part)
		}
	}
	return strings.Join(parts, ".")
}

// Go errors are defined in third_party/go/src/syscall/zx/fidl/errors.go
var goErrorCodeNames = map[gidlir.ErrorCode]string{
	gidlir.StringTooLong:               "ErrStringTooLong",
	gidlir.StringNotUtf8:               "ErrStringNotUTF8",
	gidlir.NonEmptyStringWithNullBody:  "ErrUnexpectedNullRef",
	gidlir.StrictUnionFieldNotSet:      "ErrInvalidXUnionTag",
	gidlir.StrictUnionUnknownField:     "ErrInvalidXUnionTag",
	gidlir.InvalidPaddingByte:          "ErrNonZeroPadding",
	gidlir.StrictEnumUnknownValue:      "ErrInvalidEnumValue",
	gidlir.StrictBitsUnknownBit:        "ErrInvalidBitsValue",
	gidlir.ExceededMaxOutOfLineDepth:   "ErrExceededMaxOutOfLineDepth",
	gidlir.InvalidNumBytesInEnvelope:   "ErrInvalidNumBytesInEnvelope",
	gidlir.InvalidNumHandlesInEnvelope: "ErrInvalidNumHandlesInEnvelope",
	gidlir.ExtraHandles:                "ErrTooManyHandles",
	gidlir.TooFewHandles:               "ErrNotEnoughHandles",
	gidlir.NonResourceUnknownHandles:   "ErrValueTypeHandles",
}

func goErrorCode(code gidlir.ErrorCode) (string, error) {
	if str, ok := goErrorCodeNames[code]; ok {
		return fmt.Sprintf("fidl.%s", str), nil
	}
	return "", fmt.Errorf("no go error string defined for error code %s", code)
}
