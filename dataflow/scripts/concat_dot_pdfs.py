#!/usr/bin/env python3
import os
import sys
import subprocess
from PyPDF2 import PdfMerger

def main(subdir, prefix):
    # Find all .dot files with the given prefix in the subdirectory
    dot_files = [f for f in os.listdir(subdir) if f.startswith(prefix) and f.endswith('.dot')]
    dot_files.sort()

    pdf_files = []

    for dot_file in dot_files:
        dot_path = os.path.join(subdir, dot_file)
        pdf_file = dot_file[:-4] + '.pdf'  # Replace .dot with .pdf
        pdf_path = os.path.join(subdir, pdf_file)
        # Render .dot file to PDF using Graphviz
        subprocess.run(['dot', '-Tpdf', dot_path, '-o', pdf_path], check=True)
        pdf_files.append(pdf_path)

    # Merge all PDFs into a single PDF
    merger = PdfMerger()

    for pdf_file in pdf_files:
        merger.append(pdf_file)

    output_pdf = os.path.join(subdir, f"{prefix}_merged.pdf")
    merger.write(output_pdf)
    merger.close()

    print(f"Merged PDF saved to {output_pdf}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python concat_dot_pdfs.py <subdir> <prefix>")
        sys.exit(1)

    subdir = sys.argv[1]
    prefix = sys.argv[2]
    main(subdir, prefix)
