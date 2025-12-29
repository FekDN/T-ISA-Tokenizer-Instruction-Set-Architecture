# T-ISA-Tokenizer-Instruction-Set-Architecture
T-ISA is an open standard for the binary description of a universal tokenization pipeline. It replaces imperative, language-specific code (e.g., in Python/Rust) with a declarative set of atomic instructions (bytecode) executed by a universal virtual machine (TISAVM).

---

## 1. Design Philosophy

Traditionally, tokenizers are distributed as unique program code. This approach creates significant challenges related to portability, versioning, and security.

**T-ISA solves this through three core principles:**
1.  **Atomicity**: Complex pipelines (e.g., `BertNormalizer`) are decomposed into elementary, unambiguous operations: "Convert to Lowercase," "Normalize Unicode," "Filter by Category Mn."
2.  **Isolation of Logic and Data**: The Vocabulary (Vocab), Merge Table (Ranks), and Scores are treated as external, data-only **resources**. The T-ISA manifest is the **algorithm** that describes how to use them. Resources can be shipped as separate files or bundled within a parent container format (e.g., `.nac`) for full portability.
3.  **Unified Runtime**: A single, reference implementation of the virtual machine can execute **any** T-ISA compliant tokenizer (e.g., Llama, BERT, T5) without changing the runtime's core code, guaranteeing consistent behavior across all platforms.

---

## 2. Architectural Overview: The Five Engines

The text processing pipeline in T-ISA is divided into five sequential functional zones, or "engines," each corresponding to a specific range of opcodes.

### `0x01-0x0F`: TRANSFORM (Normalization)
Operates at the "string -> string" level on the entire input.
- **Instructions**: `LOWERCASE`, `UNICODE_NORM(form)`, `REPLACE(pattern, content)`, `FILTER_CATEGORY(UnicodeCats)`, `PREPEND(prefix)`.
- *Example (BERT)*: A chain of `REPLACE` -> `UNICODE_NORM(NFD)` -> `FILTER_CATEGORY(Mn)`.

### `0x10-0x1F`: PARTITION & FRAG_TRANSFORM (Pre-Tokenization)
Operates at the "string -> list of fragments" level and applies transformations to each fragment.
- **`0x10 PARTITION_RULES(rules[])`**: Splits the input string into fragments based on a set of rules (often regex). It isolates special tokens by marking them as `protected: true` to exempt them from further processing. Supports behaviors like `REMOVE` (for whitespace) and `ISOLATE` (for punctuation).
- **`0x15 BYTE_ENCODE`**: Applies transformations *inside* each **unprotected** fragment. This is primarily used for byte-level tokenization schemes, such as mapping raw bytes to Unicode characters (as in GPT-2).

### `0x20-0x2F`: ENCODE (Core Model)
The core of tokenization, converting text fragments into numerical IDs.
- **Instructions**: `BPE_ENCODE` (`0x20`), `WORDPIECE_ENCODE` (`0x21`), `UNIGRAM_ENCODE` (`0x22`).
- **Methods**:
  - **BPE**: Iteratively merges the best-ranked pairs based on the `ranks` resource.
  - **WordPiece**: Greedily finds the longest possible substring that exists in the vocabulary.
  - **Unigram**: Finds the most probable token sequence using the Viterbi algorithm and `scores` resource.

### `0x30-0x3F`: COMPOSE (Post-Processing)
Assembles the final sequence of IDs.
- **Instruction**: `COMPOSE(template)`.
- **Logic**: Constructs the final ID array from a template of `SLOT` elements (where the output of the ENCODE engine is inserted) and `FIXED` elements (for inserting specific token IDs like `[CLS]` or `</s>`).

---

## 3. Binary Format Specification

A T-ISA manifest is a contiguous stream of instructions:

| Offset | Length (bytes) | Field          | Description                                                    |
| :---   | :---           | :---           | :---                                                           |
| 0      | 4              | **Magic**      | The constant string `TISA` (0x54, 0x49, 0x53, 0x41).             |
| 4      | 1              | **Version**    | The standard's version number (e.g., `0x01` for v1.0).           |
| *Start of Stream* | | | |
| +0     | 1              | **Opcode**     | The operation code (e.g., `0x01` - `0x30`).                      |
| +1     | 4              | **Payload Length**| The length of the `Payload` in bytes (UInt32, Little-endian). |
| +5     | Variable       | **Payload**    | Operation arguments, serialized as a UTF-8 encoded JSON string in v1.0. |
| *End of Stream*   | | | |

---

## 4. Implementation Examples

### GPT-2 Manifest (BPE + ByteLevel)
```json
// --- PIPELINE ---
// 1. [PARTITION] 0x10 PARTITION_RULES: Isolate the special token '<|endoftext|>'
//    and then split the rest of the text into words, spaces, and punctuation
//    using a complex regex.
//    (payload: {
//      rules: [
//        {pattern: "<\\|endoftext\\|>", protected: true},
//        {pattern: "'s|'t|...| ?\\p{L}+", regex: true}
//      ]
//    })
//
// 2. [FRAG_TRANSFORM] 0x15 BYTE_ENCODE: Convert each non-protected fragment's
//    UTF-8 bytes into a sequence of mappable Unicode characters.
//    (payload: { pipeline: [ {type: "BYTE_ENCODE"} ]})
//
// 3. [ENCODE] 0x20 BPE_ENCODE: Apply the Byte-Pair Encoding (BPE) merge
//    algorithm to the character sequences.
//    (payload: {})
```

### T5 Manifest (Unigram + SentencePiece)
```json
// --- PIPELINE ---
// 1. [TRANSFORM] 0x03 REPLACE: Replace all space characters with ' ' (U+2581).
//    (payload: {pattern: " ", val: "\u2581"})
//
// 2. [TRANSFORM] 0x07 PREPEND: Add a leading ' ' to the beginning of the string.
//    (payload: {val: "\u2581"})
//
// 3. [PARTITION] 0x10 PARTITION_RULES: Isolate all special tokens
//    (e.g., <pad>, </s>, <extra_id_...>) so they are not processed further.
//    The 'trim_preceding_space' parameter removes a leading space marker
//    before a special token, ensuring correct tokenization.
//    (payload: {
//      rules: [
//        {pattern: "<extra_id_40>", protected: true, trim_preceding_space: "\u2581"},
//        ... (100+ similar rules for other special tokens)
//      ]
//    })
//
// 4. [ENCODE] 0x22 UNIGRAM_ENCODE: Apply the Unigram (Viterbi) algorithm to find
//    the most likely token sequence for the remaining fragments.
//    (payload: {})
//
// 5. [COMPOSE] 0x30 COMPOSE: Append the End-of-Sequence token '</s>' to the final list of IDs.
//    (payload: { template: [ ["SLOT"], ["FIXED", "</s>"] ]})
```

---

## 5. Industry Advantages

1.  **True Cross-Platform Portability**: A developer can publish a `tokenizer.tisa` file alongside model weights, enabling instant, code-free integration with any compliant runtime (e.g., in C++, Rust, or Mojo).
2.  **Enhanced Security**: The declarative, non-executable manifest prevents arbitrary code execution, eliminating the risk of Remote Code Execution (RCE) vulnerabilities present in script-based tokenizers.
3.  **Guaranteed Bit-for-Bit Accuracy**: The standard ensures that the output IDs are identical to the reference implementation, providing perfect replicability.
4.  **Streaming-Ready**: The architecture is designed for future extension with state management instructions (`SAVE_STATE`/`RESTORE_STATE`), enabling correct tokenization of data streams without losing characters at chunk boundaries.
5.  **Creation from Scratch**: The declarative format allows developers to design and fine-tune custom tokenizers using a simple JSON configuration, without needing to write complex code.

---

## 6. Core Components

-   **TISACompiler**: A parser that analyzes an original tokenizer's implementation and configuration (e.g., from `transformers`) to produce a binary T-ISA manifest.
-   **TISAVM**: An interpreter that executes a binary T-ISA manifest to perform tokenization on input text.

---
---

### Project Status & Validation

The accompanying `tests.py` script serves as a comprehensive test suite to validate the T-ISA specification and its reference implementation. It automatically compares the output of the TISAVM against the original Hugging Face tokenizer for dozens of popular and diverse models, including variants of BERT, GPT, T5, Llama, RoBERTa, BLOOM, and more. This ensures that the compiled manifests produce bit-for-bit identical results to their reference counterparts.

It is important to note that T-ISA is currently a proof-of-concept. While it has demonstrated perfect parity across a wide range of tested models, the architecture requires further testing on an even broader set of tokenizers and edge cases. Future work may involve architectural refinements to support emerging tokenization techniques.

## License

The source code of this project is licensed under the **Apache License 2.0**. A full copy of the license is available in the `LICENSE` file in the root directory of this repository and can also be viewed at [https://www.apache.org/licenses/LICENSE-2.0](https://www.apache.org/licenses/LICENSE-2.0).
---
*   Dmitry Feklin
*   feklindn@gmail.com
*   2025
---

