if exists("b:current_syntax")
  finish
endif

syntax keyword gtiConditional else if
syntax keyword gtiRepeat for while
syntax keyword gtiDeclaration class include mut namespace
syntax keyword gtiStatement return
syntax keyword gtiOperator and or self
syntax keyword gtiType bool expected float int string void
syntax keyword gtiKeyword unexpected
syntax keyword gtiBoolean false true
syntax keyword gtiConstant nullptr

syntax match gtiNumber "\<\d\+\>"
syntax match gtiFloat "\<\d\+\.\d\+\>"
syntax match gtiOperator "\(::\|==\|!=\|<=\|>=\|++\|--\|+=\|-=\|[+*/<>=!-]\)"
syntax match gtiComment "//.*$" contains=@Spell
syntax match gtiPreProc "@runtime"
syntax match gtiPreProc "#\(if\|elif\|else\|endif\)\>"
syntax match gtiCompileTarget "\<target\.\(os\|vendor\|arch\)\>"
syntax match gtiAttribute "\[\[discard\]\]"
syntax region gtiString start=+"+ skip=+\\"+ end=+"+

highlight default link gtiAttribute PreProc
highlight default link gtiBoolean Boolean
highlight default link gtiComment Comment
highlight default link gtiConditional Conditional
highlight default link gtiConstant Constant
highlight default link gtiCompileTarget Special
highlight default link gtiDeclaration Keyword
highlight default link gtiFloat Float
highlight default link gtiKeyword Keyword
highlight default link gtiNumber Number
highlight default link gtiOperator Operator
highlight default link gtiPreProc PreProc
highlight default link gtiRepeat Repeat
highlight default link gtiStatement Statement
highlight default link gtiString String
highlight default link gtiType Type

let b:current_syntax = "gti"
