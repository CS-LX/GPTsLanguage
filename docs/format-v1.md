# GPT Assembly v1

GPT Assembly is a Windows x64 PE32+ DLL profile. A Windows caller uses ordinary native exports. A GPT reader uses the metadata and UTF-8 paragraph bodies. All program operations, including comparisons and branches, are defined by the paragraphs. A reader can execute them without loading the PE, consulting an LLM, or recovering the source program.

## Container and method table

The COFF machine is AMD64, the DLL flag is set, and the CLR directory is empty. Two unique initialized read-only data sections are required. Use the section's VirtualSize for the payload length. In a file, take the bytes at PointerToRawData. In a mapped image, take them at VirtualAddress. File-alignment padding is outside the payload.

`.gptmeta` starts with the eight ASCII bytes `GPTASM\r\n`, a little-endian u16 format version (1), a u16 English grammar version (1 or 2) and a u32 method count. Each record below follows the previous one immediately. Integers are little-endian, with no padding.

| Field | Representation |
| --- | --- |
| Method ID | u32, sequential from 0 in source order |
| Body offset and length | Two u32 byte counts relative to the start of `.gptil` |
| Export name length | u16 byte count |
| Signature | Four bytes: result type 1, arity 2, parameter type 1, parameter type 1. Type 1 means u32 |
| Export name | Exactly the specified number of ASCII bytes, C identifier, case-sensitive, unique |

Both sections end exactly after their contents. Bodies are nonempty, contiguous in method order and have no NUL terminator or inter-method separator. The method table gives every boundary. This profile supports 1–1024 methods, up to 16 MiB per custom section and 65535 instructions per method.

## English grammar and evaluation

One paragraph is one instruction. Separate paragraphs with exactly two LF bytes. Punctuation, spacing, and case are literal. The dot is U+00B7 and the dash is U+2014. Angle-bracket placeholders below are replaced by the indicated text. Numbers are canonical unsigned decimal with no leading zero apart from zero itself.

| Operation | Exact paragraph |
| --- | --- |
| Read parameter | Context · Inputs · Alignment. Before we go any further, it helps to establish a shared starting point. For this part of the discussion, place argument <N> on the stack. |
| Push constant | A little context goes a long way here. To keep the discussion grounded in something concrete, place the value <N> on the stack. |
| Add or subtract | To be clear, acknowledging a second contribution does not invalidate the first. With that distinction in mind — and this is where the nuance matters — the practical takeaway is to <verb> the earlier value by the latest contribution. |
| Compare | It is worth pausing here to avoid a common misunderstanding. The question we actually need to answer is whether the earlier value is <relation> the later one. |
| Branch on false | But this does not mean we should dismiss the alternative altogether. If that assessment was false, resume the discussion at paragraph <N>. |
| Jump | With that context in place, we can move the conversation forward. The next part worth your attention begins at paragraph <N>. |
| Return | Ultimately, the caller came here with a specific question. The most helpful thing we can do at this point is close the loop and return the remaining value. |

The stack starts empty. Parameter numbers are 1 and 2. Constants range from 0 through 4294967295. A binary operation pops the latest (right) operand and then the earlier (left) one, and pushes one result. The verbs are `increase` (addition) and `decrease` (subtraction), modulo 2³². Valid relations are `equal to`, `different from`, `less than`, `greater than`, `at most` and `at least`. Comparisons use unsigned values and yield 0 or 1.

A conditional branch pops one value, jumping on zero and otherwise advancing. An unconditional jump consumes nothing. Targets are one-based paragraph numbers in the current method and must be strictly forward. Return requires exactly one stack value and ends that call. All paragraphs must be reachable in the control-flow graph, incoming stack heights must agree at merges, and every path must return. Unknown grammar, underflow, fall-through at method end and invalid targets are format errors.

### English grammar v2

Version 2 adds three instructions to the existing English grammar. The container and native calling convention are the same. The compiler selects grammar 2 when a module uses a short-circuit gate. A reader must reject a v2 instruction in a module labeled grammar 1.

| Operation | Exact paragraph |
| --- | --- |
| AND gate | To be clear, the second point may be perfectly valid in its own right. It can be thoughtful. It can be nuanced. It can even be correct. But if the first premise is false, none of that changes the answer — record zero and resume at paragraph <N>. |
| OR gate | You're absolutely right to ask for a second opinion. That said, if the first point already holds, a second opinion won't make it any more true — record one and resume at paragraph <N>. |
| Normalize | There's a lot of nuance in that number, and I don't want to flatten your experience. For the purposes of this particular yes-or-no question, though, any nonzero value counts as one. Zero speaks for itself. |

Both gates require a left operand on the stack. AND jumps on zero; OR jumps on nonzero. The taken edge replaces the top value with 0 (AND) or 1 (OR) and jumps directly to the indicated one-based paragraph. The fall-through edge pops the left operand and enters the right-hand expression. Normalize converts its top value to 0 or 1 in place. Both edges arrive at the merge with one Boolean result above the original stack prefix.

`gptc trace FILE NAME A B` runs the same evaluator as the native dispatcher. For each visited paragraph it prints the original file text, the stack after the operation (rightmost is top), and the one-based inclusive range of any skipped paragraphs. The final line gives the return value and the number of paragraphs executed.

## Native ABI

Each public name exports a Windows x64 function of type `uint32_t(uint32_t, uint32_t)`. Its only job is to tail-call the imported `gpt_dispatch` from `gptrt.dll`, with arguments (mapped module base, method ID, first u32, second u32). The entry address of the DLL is zero. The emitter puts only bridges in the executable section. The interpreter lives in the separate runtime. Windows loads that dependency in the usual way.

The v1 inspector checks the native bridge profile as well as the paragraphs. The sole executable section is a read-only `.text` with exactly 24 bytes per method, in ID order. Each bridge performs the five operations below. Immediates and displacements occupy four little-endian bytes. The base is RVA zero; each displacement is relative to the end of its instruction. The named PE exports are sorted by ASCII name, with export address-table index equal to the GPT method ID. Only the runtime dispatcher is imported, through a normal import descriptor with an original thunk table and a two-slot IAT (address and terminator). There is no TLS initializer.

| Offset | Bytes | Effect |
| --- | --- | --- |
| 0 | `41 89 D1` | Copy EDX (second user argument) to R9D |
| 3 | `41 89 C8` | Copy ECX (first user argument) to R8D |
| 6 | `BA imm32` | Put method ID in EDX |
| 11 | `48 8D 0D disp32` | Put this mapped image base in RCX |
| 18 | `FF 25 disp32` | Tail-call the runtime through its IAT slot |

A successful native call clears Win32 last-error, including when the function returns zero. Invalid modules yield zero and set last-error to ERROR_BAD_FORMAT. Read last-error immediately; the runtime's optional `gpt_last_error()` export returns a thread-local UTF-8 diagnostic valid until the next GPT call on that thread. Runtime and inspector validate every method before evaluation.

## Source subset

The source front end also accepts unary `!` and the left-to-right short-circuit operators `&&` and `||`. Precedence descends from unary negation through arithmetic, ordering, equality, logical AND and finally logical OR. Logical results are always 0 or 1. Unary negation alone lowers to grammar 1.

Source files contain one or more definitions, each with two named u32 parameters and a u32 result. Type spellings are `uint32_t` (with `<stdint.h>`) and `unsigned int` on the x64 target. The compiler also accepts an optional C++ `extern "C"` on a definition. Statements are blocks, `if` with optional `else`, and `return`. Expressions support parameters, decimal u32 constants with optional `u` suffix, parentheses, binary `+`, `-`, all six ordinary comparisons, and C precedence/left associativity. Every path must return. There are no implicit external calls or native fallbacks. This is a deliberately bounded C/C++ subset, not a general C preprocessor.

Unsuffixed literals must fit a signed 32-bit int. Use, for example, `4294967295u` for larger u32 literals. Ordering two signed subexpressions uses C's signed comparison; the compiler biases both values by 2³¹ before emitting the unsigned VM comparison. Source results are defined where the original C/C++ expression has defined behavior. The front end rejects unreachable statements and source nesting over 256 parser levels.
