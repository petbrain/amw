# AMW: another markup way

In prevalent terms AMW is a markup language, but that's not quite correct in terms of common sense.

AMW is inspired by YAML, but strictly speaking, YAML is not a language, it's rather a way of markup.
Moreover, it's a superset of JSON where N stands for Notation.

Thus, AMW is yet another markup way. Same as YAML, it's pimarily intended for human-generated data.

## File extension

Let it be `.amw`

Someone used or uses this extension but they haven't managed to get on wikipedia with it so I take it.
I doubt AMW is shitty enough for humans to become popular so no one will be offended.

## Motivation

* I always forget what `>` and `|` in YAML stand for, not mentioning other cryptic stuff.
* Need for clear parsing rules.
* Need for clear data type specification.
* Need for more data types, e.g. dates.

## Parsing

The parser processes input stream line by line.
All trailing space characters are stripped off by line reader before conveying lines to the parser.

The basic structural element of the markup is block whose bounds are defined by indentation.

The root block has zero indent. Nested blocks start from greater indent and end when it goes lower.

Only space characters can be used for indentation.
Tabs may break formatting especially if used in map keys.
Avoid using tabs.

## Data types

* null
* boolean
* signed integer
* unsigned integer
* float
* date/time
* timestamp
* string
* list
* map

`list` and `map` are complex types that require recursive parsing, controlled by indentation rules.
All other types are considered simple.

## Conversion specifier

The first line of the block may contain conversion specifier. Otherwise, type deduction rules apply.

Conversion specifier is a string enclosed by colons and followed by space or end of line.
The following conversions are supported:

* `:raw:` return literal string as is, without dedent
* `:literal:` parse value as literal string
* `:folded:` parse value as string and fold it, same as in YAML
* `:datetime:` parse value as datetime
* `:timestamp:` parse value as timestamp in the form seconds\[.frac\], up to nanosecond resolution
* `:json:` parse value as JSON

Custom conversion routines can be set with `amw_set_custom_parser` function.

## Simple types

Simple types are always single-line, except strings:
```yaml
100  # numeric value

:datetime: 2012-01-01  # end of world the God forgot about

:datetime:
  2012-01-01

answer:
  # numeric value:
  42
```

### DateTime

AMW supports formats mutual of ISO-8601 and RFC 3339
(see https://ijmacd.github.io/rfc3339-iso8601/).

In addition, T or space separator between date and time and dashes in the date part are optional.

### Numbers

Positive integers returned as signed if they are within positive range of 64-bit signed value.
Otherwise they are returned as unsigned.

## Strings

Strings may span multiple lines. There are two types of strings: literal and quoted.

### Literal strings

Literal strings may span multiple lines.
All line breaks are included into the parsed value including the final one.

Note that single-line literal strings do not contain line break.

As long as the block ends only when a line has lower indent,
lines may have greater individual indents within the block.
```
+--- block indent is zero for the root block
|
v
# leading comment
       Lorem ipsum dolor sit amet,
    consectetur adipiscing elit,
         sed do eiusmod tempor incididunt
     ut labore et dolore magna aliqua.
```

The resulting text is dedented when the block is fully read:
```
   Lorem ipsum dolor sit amet,
consectetur adipiscing elit,
     sed do eiusmod tempor incididunt
 ut labore et dolore magna aliqua.

```

If the string has conversion specifier and follows it on the same line,
the block indent is bumped to the end of conversion specifier:
```
+--- initial block indent
|
v
# leading comment

          +--- bumped block indent
          |
          v
:literal:     Lorem ipsum dolor sit amet,
          consectetur adipiscing elit,
          sed do eiusmod tempor incididunt
          ut labore et dolore magna aliqua.
```

If conversion specifier is immediately followed by line break, the block indent is increased by one.
As already mentioned, lines may have greater indent. The resulting text is dedented anyway.
```
+--- initial block indent
|+--- increased block indent for the block that follows conversion specifier
||  +--- actual indent, the text will be dedented
||  |
vv  v
:literal:
        Lorem ipsum dolor sit amet,
    consectetur adipiscing elit,
    sed do eiusmod tempor incididunt
    ut labore et dolore magna aliqua.
```

The following markup is prohibited. It's neither aesthetic nor simple to parse.
```
:literal: Lorem ipsum dolor sit amet,
    consectetur adipiscing elit,
    sed do eiusmod tempor incididunt
    ut labore et dolore magna aliqua.
```

Literal strings cannot start with numbers and reserver keywords `null`, `true`, `false`:
```
distance: 25.5 miles # bad markup
error: null pointer  # bad markup
```

### Quoted strings

Quoted strings follow similar indentation rules.
The block indent for multi-line strings is set next to the opening quote:
```
+--- initial block indent
|
v
# leading comment

 +--- block indent for quoted string
 |
 v
"Lorem ipsum dolor sit amet,
 consectetur adipiscing elit"
```

The closing quote may have the same indent as the opening one but no less:
```
    "Lorem ipsum dolor sit amet,
     consectetur adipiscing elit
    "
```

Multi-line quoted strings are always folded.
They are dedented and empty lines are replaced with line breaks:
```
 +--- block indent
 |
 v
"
   Lorem

     ipsum
   dolor

   sit

"
```

The resulting value will be: `Lorem\n\n  ipsum dolor\nsit`.
Note double space after the first token.

If line breaks, heading or trailing spaces, quotes and other special characters are required, use escapes:
```
"Lorem ipsum dolor sit amet,\n
 consectetur adipiscing elit\n"
```
All escapes defined for JSON https://www.rfc-editor.org/rfc/rfc8259 are supported.

Unlike multiple-line, single-line quoted strings are not trimmed and all spaces are preserved,

### Folded strings

Folding rules:
1. Lines are dedented.
2. When concatenated, they are separated with space unless a string already starts with space.
3. Empty lines are replaced with line breaks.


## Lists

List items start from hyphen followed by space.
Each item must begin on a new line:
```
- 1
- 2
- 3
- a
- b
- c
```

Unlike YAML, inline lists are not supported natively.
However, they can be expressed in JSON using conversion specifier:
```
:json: [1, 2, 3, "a", "b", "c"]
```

The parser treats list item as a nested markup and the block indent
is increased by two spaces:
```
  +--- block indent
  |
  v
- Lorem ipsum dolor sit amet,
  consectetur adipiscing elit
  
- sed do eiusmod tempor incididunt
  ut labore et dolore magna aliqua.
```

Nested list example:
```
  +--- block indent for the root list item
  | +--- block indent for nested list items
  | |
  v v
- - sed do eiusmod tempor incididunt
  - ut labore et dolore magna aliqua.
```

## Maps

Map keys can be one of the following types:

* null
* boolean
* integer
* float
* string

Keys are separated from values by a colon followed by space character, conversion specifier, or newline.
In particular, this means that URLs do not have to be quoted:
```
https://www.example.com: good key/value pair
http://www.example.net : also good key/value pair, the trailing
                         space in key is stripped
http://www.example.org:
    another good key/value pair
```

Keys are always single-line values, even if they are strings, no matter literal or quoted.
Literal strings may contain spaces:
```
some key: and value
```

If a key has to contain colons followed by space, it must be quoted:
```
"key : with : colons": such a key must be quoted
```

Again, if the key is a literal string, it cannot span multiple lines:
```
this will be: interpreted as single-line key-value pair,
not as a multi-line literal string
```

If the key is a quoted string, it simply cannot end with a colon:
```
"this cannot
 be a key": and this garbage will cause parse error
```

## Comments

As in YAML, comments start from `#`.
There are some rules where comments can appear.

Everything that follows conversion specifier is treated as a raw value.
Therefore, comments are not allowed immediately after conversion specifier.
But they can follow simple values or be a part of the block if conversion specifier
is followed by line break:
```
:float: 100  # numeric value

:float:
  # numeric value
  100
```

However, if a value is an explicitly denoted literal text or raw value, the comment will be
a part of it:
```
- :raw: # this is not a comment
- :literal: # this is also a part of string, not a comment
```

Comments may appear in the beginning of block:
```
+--- initial block indent
|
v
# this is a comment

          +--- bumped block indent after detecting conversion specifier
          |
          v
:literal: Lorem ipsum dolor sit amet,
          consectetur adipiscing elit,
          sed do eiusmod tempor incididunt
          ut labore et dolore magna aliqua.
```

Comments can appear immediately after map keys:
```
    +--- block indent of nested block
    |
    v
some key: # this comment is handled when the key is parsed
    # this comment is processed as a part of nested block
    :literal: # this is not a comment, this is a part of string
              Lorem ipsum dolor sit amet,
              consectetur adipiscing elit,
              sed do eiusmod tempor incididunt
              ut labore et dolore magna aliqua.
              ^
              |
              +--- bumped block indent after detecting conversion specifier
```

If comments appear after list item specifiers, they are processed as a part of nested block:
```
  +--- block indent of nested block
  |
  v
- # this comment is a part of nested block
  100  # another comment
```

The indent of comments can be less than block indent.
This allows commenting out parts of literal or quoted strings:
```
          +--- block indent
          |
          v
:literal: Lorem ipsum dolor sit amet,
          consectetur adipiscing elit,
#          sed do eiusmod tempor incididunt
          ut labore et dolore magna aliqua.
          
 +--- block indent
 |
 v
"Lorem ipsum dolor sit amet,
 consectetur adipiscing elit,
# sed do eiusmod tempor incididunt
 ut labore et dolore magna aliqua.
"
```

### More comment examples

```
success: true  # this is a comment
greeting: "Hello!" # this is a comment
greeting: Hello! # this is a part of literal string
distance: 25.5  # this is a comment
```

### Comments in JSON

Comments are allowed in multi-line JSON after any element:
```
{   # sample object

    "foo"  # this is a key
    :      # separator
    "bar"  # this is a value
}
```

## Type deduction rules

* `null` optionally followed by `#` or `:<SP>` or `:<LF>`: null value, otherwise it's a literal string
* `true` | `false` optionally followed by `#` or `:<SP>` or `:<LF>`: boolean value, otherwise it's a literal string
* `+` | `-` followed by parseable number which in turn is followed by `#` or `:<SP>` or `:<LF>`: numeric value;
  if followed by anything else, it's a literal string
* `"|'`: quoted string
* `-<SP>`: list item
* `:<SP>` or `:<LF>` in a literal string: key/value pair of a map
