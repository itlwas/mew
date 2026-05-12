# MEW - pocket reference (v1.0)

## Lexical
- Comment: `#` to end of line
- Numbers: `123`  `3.14`  `1.5e10`  (always float64; `-5` is unary minus)
- Strings: `"abc"`, escapes `\n \t \r \" \\ \0`
- Identifier: letter or `_`, then letters/digits/`_`

## 17 keywords
`if then else end while do for to in fn return break and or not true false nil`

## 7 types
`nil`  `bool`  `number`  `string`  `list`  `map`  `fn`

## Literals
```
nil   true   false   42   3.14   "hi"
[1, 2, 3]                    # list, indices 0..n-1
{"k": 1, "x": "y"}           # map, keys are string literals only
fn(x) return x*x end         # anonymous function
```

## Expressions
```
a+b a-b a*b a/b a%b          # numbers
a..b                         # concatenation: string..string or list..list
a==b a!=b a<b a>b a<=b a>=b  # numbers or strings
a and b   a or b   not a     # short-circuit
-a                           # unary minus
x[k]   x.k                   # indexing, x.k is equivalent to x["k"]
f(a,b)                       # call
```

## Precedence (low to high)
`or` < `and` < `not` < comparisons < `+ - ..` < `* / %` < unary `-` < `[] . ()`

## Statements
```
x = expr                     # assignment
x[k] = expr                  # index write
x.k = expr                   # field write
if e then ... else ... end
while e do ... end
for i = a to b do ... end    # i takes a, a+1, ..., b; skipped if a > b
for x in col do ... end      # list: values, map: keys (sorted), string: chars
fn name(a,b) ... end         # named function
return e                     # e is optional
break                        # exit the nearest loop
```
Blocks do not create a scope, functions do. Assignment walks up the scope chain and writes to the binding found, or creates one in the current scope.

## Truthiness
Only `false` and `nil` are falsy. Everything else is truthy (including `0` and `""`).

## Builtins (53)
```
# io
print(...) write(...) repr(x) read() -> string|nil

# types and conversion
len(x) type(x) str(x) num(x) -> number|nil

# strings
upper lower trim repeat(s,n) starts_with(s,p) ends_with(s,p) contains(s|list|map, x)
split(s,sep) join(list,sep) find(s,sub) slice(x,i,j) replace(s,a,b) reverse

# lists
push(l,x) pop(l) append(a,b) reverse(l) sort(l) sort(l, cmp_fn)

# maps
keys(m) values(m) has(m,k) del(m,k) contains(m,k)

# files
read_file(path) -> string|nil   write_file(path,data) -> bool
lines(path) -> list<string>|nil

# environment
args() exit(code) getenv(name) clock() -> sec  time() sleep(sec)

# math
abs min max floor ceil round sqrt pow(x,y) rand() rand(n) seed(n)

# bytes and formatting
chr(n) -> string    ord(s) -> number
format(fmt, ...) -> string     # %s %d %x %f, width %5d, zero-pad %05d, precision %.2f

# errors and modules
error(msg) assert(cond) assert(cond, msg) load(path)
```

## Semantics
Top-down, left-to-right execution. Nothing implicit. Numbers are float64. Strings are byte sequences. Errors abort the program with file, line and reason. Recursion depth is capped at 512. The parsed AST lives for the whole process (function bodies need it). Code reads the way it runs.
