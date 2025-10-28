# Ncode Paper App 3

Convert regular PDF files into Ncode-embedded PDFs for use with NeoSmartpen devices.

## Quick Start

### Basic Usage

Convert a PDF with default settings:
```cmd
ncode-paper-app-3.exe input.pdf
```

This will:
- Read `input.pdf`
- Look for Ncode patterns in the `Ncode paper` folder
- Create `input_ncode.pdf` in the same directory

### Custom Pattern Folder

Specify a different pattern folder:
```cmd
ncode-paper-app-3.exe input.pdf "C:\Path\To\Patterns"
```

## Requirements

- Windows 64-bit
- Ncode pattern PDFs (included in `Ncode paper` folder)
- Input PDF to convert

## Pattern Files

The `Ncode paper` folder should contain pattern PDFs named like:
- `1_*.pdf` (for page 1)
- `2_*.pdf` (for page 2)
- `3_*.pdf` (for page 3)
- etc.

The app will automatically match pattern files to input pages by number.

## Command Line Options

```
ncode-paper-app-3.exe [input_pdf] [pattern_directory]
```

**Parameters:**
- `input_pdf` - Path to the PDF file to convert (default: `1_ROCF_C.pdf`)
- `pattern_directory` - Path to folder containing Ncode pattern PDFs (default: `Ncode paper` in same directory as .exe)

## Examples

### Example 1: Convert with default pattern folder
```cmd
ncode-paper-app-3.exe myDocument.pdf
```

### Example 2: Convert with custom patterns
```cmd
ncode-paper-app-3.exe myDocument.pdf "D:\MyPatterns"
```

### Example 3: Using relative paths
```cmd
ncode-paper-app-3.exe "..\input\test.pdf" "..\patterns"
```

## Output

The app creates a new PDF file with `_ncode` suffix:
- Input: `document.pdf`
- Output: `document_ncode.pdf`

The output PDF will have:
- Original content from input PDF
- Ncode patterns embedded in black (K channel)
- A4 size pages (595 x 842 points)
- 600 DPI rendering

## Technical Details

- **Resolution**: 600 DPI
- **Page Size**: A4 (210mm x 297mm)
- **Color Space**: CMYK (background in CMY, patterns in K)
- **Compression**: Enabled for smaller file sizes

## Troubleshooting

### "Pattern not found for page X"
Ensure your `Ncode paper` folder contains a PDF starting with `X_` (e.g., `3_pattern.pdf` for page 3)

### "Cannot open include file: 'mupdf/fitz.h'"
This is a compile error, not a runtime error. The .exe is already compiled and doesn't need MuPDF headers.

### "Failed to open graphics PDF"
Check that the input PDF path is correct and the file exists.

## Building from Source

To rebuild this application:
1. Install Visual Studio 2022 with C++ support
2. Build MuPDF library (Debug or Release)
3. Configure include/library paths in project settings
4. Set C++ standard to C++17
5. Build in Release x64 configuration

## Version Info

- Build Configuration: Release x64
- Compiler: MSVC v143 (Visual Studio 2022)
- Platform Toolset: v143
- C++ Standard: C++17

## License

Check the main project repository for license information.
