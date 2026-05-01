from pypdf import PdfReader

with open("pdf_output.txt", "w", encoding="utf-8") as f:
    f.write("--- DOC 1 ---\n")
    r1 = PdfReader(r'c:\Users\User\Desktop\OS Project 2\Chrono_Rift_Docker_Guide.docx.pdf')
    for p in r1.pages:
        f.write(p.extract_text() + "\n")
    
    f.write("--- DOC 2 ---\n")
    r2 = PdfReader(r'c:\Users\User\Desktop\OS Project 2\OS-Semester-Project - CS2006 - BSCS - Spring2026 (1).pdf')
    for p in r2.pages:
        f.write(p.extract_text() + "\n")
