// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package summarize

import (
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

// enum is an enumeration element.
type enum struct {
	wraparoundType
	notMember
}

// addEnums adds the API elements associated with the bits declarations.
func (s *summarizer) addEnums(enums []fidlgen.Enum) {
	for _, e := range enums {
		for _, m := range e.Members {
			s.addElement(newIsMember(e.Name, m.Name, fidlgen.EnumDeclType))
		}
		s.addElement(
			enum{
				wraparoundType: wraparoundType{
					named:      newNamed(e.Name),
					subtype:    e.Type,
					strictness: e.Strictness,
					parentType: fidlgen.EnumDeclType,
				},
			})
	}
}
