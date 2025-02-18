// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package summarize

import (
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

const libraryKind = "library"

// library is a library element.
type library struct {
	r fidlgen.Root
	notMember
}

// String implements Element.
func (l library) String() string {
	return l.Serialize().String()
}

// Name implements Element.
func (l library) Name() string {
	return string(l.r.Name)
}

func (l library) Serialize() elementStr {
	var e elementStr
	e.Name = l.Name()
	e.Kind = libraryKind
	return e
}
