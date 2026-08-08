if exists("b:current_syntax")
  finish
endif

syntax match gtiNumber "\<\d\+\>"
syntax match gtiFloat "\<\d\+\.\d\+\>"
syntax match gtiOperator "\(\.\.\.\|::\|->\|<<\|>>\|==\|!=\|<=\|>=\|++\|--\|+=\|-=\|[+*/%&|^~<>=!-]\)"
syntax match gtiFunction "\<\h\w*\ze\s*("
syntax match gtiMember "\%(\.\|->\)\s*\zs\h\w*"
syntax match gtiNamespaceName "\<\h\w*\ze\s*::"
syntax match gtiClassName "\h\w*" contained
syntax match gtiGenericType "\%(<\s*\|,\s*\)\zs\h\w*\ze\s*[<,>]"
syntax match gtiConcept "\<std::\zs\h\w*\ze\s\+\h\w*\%(\.\.\.\)\?\s*[,>]"
syntax match gtiUserType "\<[A-Z]\w*\>"
syntax match gtiConstructorField "[,:]\s*\zs\h\w*\ze\s*("
syntax match gtiDestructor "\~\s*\zs\h\w*\ze\s*("
syntax match gtiNamespaceDeclaration "\h\w*" contained
syntax match gtiTypeAliasName "\h\w*" contained

syntax keyword gtiConditional case default else if switch
syntax keyword gtiRepeat for while
syntax keyword gtiClassKeyword class struct nextgroup=gtiClassName skipwhite
syntax keyword gtiEnumKeyword enum nextgroup=gtiClassKeyword skipwhite
syntax keyword gtiAccess public private
syntax keyword gtiNamespaceKeyword namespace nextgroup=gtiNamespaceDeclaration skipwhite
syntax keyword gtiTypeAliasKeyword using nextgroup=gtiTypeAliasName skipwhite
syntax keyword gtiStorageClass mut
syntax keyword gtiOperatorKeyword operator
syntax keyword gtiInclude include
syntax keyword gtiStatement break continue return
syntax keyword gtiOperator and or
syntax keyword gtiType auto bool char expected float int int8 int16 int32 int64 nullptr_t uint uint8 uint16 uint32 uint64 void
syntax keyword gtiKeyword unexpected
syntax keyword gtiBoolean false true
syntax keyword gtiConstant nullptr
syntax keyword gtiThis this

syntax keyword gtiTodo FIXME NOTE TODO contained
syntax match gtiComment "//.*$" contains=gtiTodo,@Spell
syntax match gtiPreProc "@runtime"
syntax match gtiPreProc "#\(if\|elif\|else\|endif\)\>"
syntax match gtiCompileTarget "\<target\.\(os\|vendor\|arch\)\>"
syntax match gtiAttribute "\[\[discard\]\]"
syntax match gtiEscape "\\." contained
syntax region gtiString start=+"+ skip=+\\"+ end=+"+ contains=gtiEscape
syntax region gtiCharacter start=+'+ skip=+\\'+ end=+'+ contains=gtiEscape
syntax match gtiStandardInclude "<std/[A-Za-z0-9_/]*>"

highlight default link gtiAttribute PreProc
highlight default link gtiAccess Keyword
highlight default link gtiBoolean Boolean
highlight default link gtiClassName Type
highlight default link gtiClassKeyword Structure
highlight default link gtiEnumKeyword Structure
highlight default link gtiCharacter Character
highlight default link gtiComment Comment
highlight default link gtiConditional Conditional
highlight default link gtiConstructorField Identifier
highlight default link gtiDestructor Function
highlight default link gtiConstant Constant
highlight default link gtiCompileTarget Special
highlight default link gtiConcept Type
highlight default link gtiEscape SpecialChar
highlight default link gtiFloat Float
highlight default link gtiFunction Function
highlight default link gtiGenericType Type
highlight default link gtiInclude Include
highlight default link gtiKeyword Keyword
highlight default link gtiMember Identifier
highlight default link gtiNamespaceDeclaration Identifier
highlight default link gtiNamespaceKeyword Structure
highlight default link gtiNamespaceName Identifier
highlight default link gtiNumber Number
highlight default link gtiOperator Operator
highlight default link gtiOperatorKeyword Keyword
highlight default link gtiPreProc PreProc
highlight default link gtiRepeat Repeat
highlight default link gtiThis Special
highlight default link gtiStatement Statement
highlight default link gtiStorageClass StorageClass
highlight default link gtiString String
highlight default link gtiStandardInclude String
highlight default link gtiTodo Todo
highlight default link gtiType Type
highlight default link gtiTypeAliasKeyword Structure
highlight default link gtiTypeAliasName Type
highlight default link gtiUserType Type

let b:current_syntax = "gti"
