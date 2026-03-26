"""
Pronoun Scraper for NewsBank PDF exports.

Usage:
    python3 scraper.py <pdf_file> [<pdf_file2> ...] [-o output.xlsx]

For each occurrence of "pronoun"/"pronouns" in the article body, outputs:
    - Article title
    - Author(s)
    - Date of publication
    - News source
    - Sentence containing the word
    - Paragraph containing the word
"""

import re
import sys
import argparse
from pathlib import Path

try:
    from pdfminer.high_level import extract_text
except ImportError:
    sys.exit("Error: pdfminer.six not installed. Run: pip3 install pdfminer.six")

try:
    import openpyxl
    from openpyxl.styles import Font, PatternFill, Alignment
except ImportError:
    sys.exit("Error: openpyxl not installed. Run: pip3 install openpyxl")


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

# Matches the date | source header line, e.g.:
#   "February 5, 2026 | EDGAR Online - Full Feed (USA)"
DATE_SOURCE_RE = re.compile(
    r'^(?P<date>\w+ \d{1,2},\s*\d{4})\s*\|\s*(?P<source>.+?)$',
    re.MULTILINE,
)

# Matches the optional author/section/words line, e.g.:
#   "Author: Elizabeth McGlonern  Section: News  426 Words"
AUTHOR_RE = re.compile(r'Author:\s*(?P<author>.+?)(?:\s{2,}|$)', re.IGNORECASE)

# Matches the word count on the date line when there's no author line, e.g.:
#   "51378 Words"
WORDS_RE = re.compile(r'^\d+ Words$', re.MULTILINE)

OPENURL_RE = re.compile(r'OpenURL Link\n', re.IGNORECASE)

# Citation block that signals end-of-article body
CITATION_RE = re.compile(r'\nCitation \(apa Style\)', re.IGNORECASE)

# Copyright lines to strip from body
COPYRIGHT_RE = re.compile(
    r'\nCopyright\s.{0,200}?\n', re.IGNORECASE | re.DOTALL
)


def parse_articles(text: str) -> list[dict]:
    """
    Split the full PDF text into individual articles and extract metadata.

    Each article dict has keys:
        title, date, source, authors, body_text
    """
    articles = []

    # Split on form-feed (\x0c) — each "page" in the PDF.
    # Articles always start on a new page, but one article can span many pages.
    pages = text.split('\x0c')

    # We accumulate pages into the current article.
    current: dict | None = None

    for page in pages:
        page = page.strip()
        if not page:
            continue

        # Detect whether this page starts a new article.
        # A new article page starts with the title (one or more lines),
        # then immediately a "Date | Source" line, then word count/author info,
        # then "OpenURL Link".
        openurl_match = OPENURL_RE.search(page)
        date_match = DATE_SOURCE_RE.search(page)

        if openurl_match and date_match:
            # Check the date line appears BEFORE OpenURL (i.e., it's in the header)
            if date_match.start() < openurl_match.start():
                # Save the previous article
                if current is not None:
                    articles.append(current)

                # Extract title: everything before the date line
                title_block = page[:date_match.start()].strip()
                title = ' '.join(title_block.splitlines()).strip()

                date_str = date_match.group('date').strip()
                source = date_match.group('source').strip()

                # Author: look between date line and OpenURL
                header_block = page[date_match.end():openurl_match.start()]
                author_match = AUTHOR_RE.search(header_block)
                authors = author_match.group('author').strip() if author_match else ''

                # Body text: everything after "OpenURL Link\n"
                body_start = openurl_match.end()
                body = page[body_start:]

                # Strip citation block (end-of-body marker)
                cite_match = CITATION_RE.search(body)
                if cite_match:
                    body = body[:cite_match.start()]

                current = {
                    'title': title,
                    'date': date_str,
                    'source': source,
                    'authors': authors,
                    'body_text': body,
                }
                continue

        # Not a new article start — append this page's text to the current article
        if current is not None:
            page_body = page

            # Strip citation if present
            cite_match = CITATION_RE.search(page_body)
            if cite_match:
                page_body = page_body[:cite_match.start()]

            current['body_text'] += '\n' + page_body

    # Don't forget the last article
    if current is not None:
        articles.append(current)

    return articles


# ---------------------------------------------------------------------------
# Text utilities
# ---------------------------------------------------------------------------

PRONOUN_RE = re.compile(r'\bpronoun(?:s)?\b', re.IGNORECASE)

# Sentence splitter: split on ". ", "! ", "? " followed by uppercase or end.
SENT_SPLIT_RE = re.compile(r'(?<=[.!?"])\s+(?=[A-Z"\'(])')

# True paragraph break: a sentence-ending character followed by one or more
# blank lines in the raw text.  We mark these with a unique token so that
# after we flatten soft line-wraps the paragraph boundaries survive.
PARA_BREAK_RE = re.compile(r'([.!?"\u201d])\s*\n')
SOFT_WRAP_RE  = re.compile(r'\n')            # remaining newlines are soft wraps


def normalize_body(raw: str) -> tuple[str, list[int]]:
    """
    Convert raw PDF body text into a clean string where:
      - True paragraph breaks  →  '¶'  (pilcrow, unlikely to appear in news)
      - Soft line-wraps        →  ' '
    Returns (clean_text, list_of_paragraph_break_positions_in_clean_text).
    """
    # Replace every newline that immediately follows a sentence-ender with ¶
    text = PARA_BREAK_RE.sub(r'\1¶', raw)
    # All remaining newlines are soft wraps → space
    text = SOFT_WRAP_RE.sub(' ', text)
    # Collapse runs of spaces
    text = re.sub(r'  +', ' ', text)
    return text


def split_sentences(text: str) -> list[str]:
    """Naive sentence splitter (operates on already-normalized text)."""
    return SENT_SPLIT_RE.split(text)


def get_paragraph(text: str, match_start: int) -> str:
    """Return the paragraph containing match_start (¶-delimited)."""
    para_start = text.rfind('¶', 0, match_start)
    para_start = 0 if para_start == -1 else para_start + 1

    para_end = text.find('¶', match_start)
    para_end = len(text) if para_end == -1 else para_end

    return text[para_start:para_end].strip()


def get_sentence(text: str, match_start: int, match_end: int) -> str:
    """Return the sentence within the paragraph that contains the match."""
    para = get_paragraph(text, match_start)
    sentences = split_sentences(para)

    # Offset of match relative to paragraph start
    para_start = text.rfind('¶', 0, match_start)
    para_start = 0 if para_start == -1 else para_start + 1
    rel_pos = match_start - para_start

    offset = 0
    for sent in sentences:
        sent_end = offset + len(sent)
        if offset <= rel_pos <= sent_end:
            return sent.strip()
        offset = sent_end + 1  # splitter consumed one space

    return sentences[-1].strip() if sentences else ''


# ---------------------------------------------------------------------------
# Main extraction
# ---------------------------------------------------------------------------

def extract_pronoun_rows(pdf_path: str) -> list[dict]:
    """Extract all pronoun hit rows from a PDF file."""
    print(f"Reading: {pdf_path}")
    text = extract_text(pdf_path)
    articles = parse_articles(text)
    print(f"  Found {len(articles)} articles")

    rows = []
    for article in articles:
        body_clean = normalize_body(article['body_text'])

        for m in PRONOUN_RE.finditer(body_clean):
            sentence = get_sentence(body_clean, m.start(), m.end())
            paragraph = get_paragraph(body_clean, m.start())

            rows.append({
                'Title': article['title'],
                'Author(s)': article['authors'],
                'Date': article['date'],
                'Source': article['source'],
                'Sentence': sentence,
                'Paragraph': paragraph,
                'PDF File': Path(pdf_path).name,
            })

    print(f"  Found {len(rows)} pronoun occurrences")
    return rows


# ---------------------------------------------------------------------------
# Spreadsheet output
# ---------------------------------------------------------------------------

COLUMNS = ['Title', 'Author(s)', 'Date', 'Source', 'Sentence', 'Paragraph', 'PDF File']

HEADER_COLOR = '1F497D'   # dark blue
HEADER_FONT_COLOR = 'FFFFFF'


def write_xlsx(rows: list[dict], output_path: str) -> None:
    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = 'Pronoun Hits'

    header_fill = PatternFill(fill_type='solid', fgColor=HEADER_COLOR)
    header_font = Font(bold=True, color=HEADER_FONT_COLOR)
    wrap = Alignment(wrap_text=True, vertical='top')

    # Header row
    for col_idx, col_name in enumerate(COLUMNS, start=1):
        cell = ws.cell(row=1, column=col_idx, value=col_name)
        cell.font = header_font
        cell.fill = header_fill
        cell.alignment = wrap

    # Data rows
    for row_idx, row in enumerate(rows, start=2):
        for col_idx, col_name in enumerate(COLUMNS, start=1):
            cell = ws.cell(row=row_idx, column=col_idx, value=row.get(col_name, ''))
            cell.alignment = wrap

    # Column widths
    widths = {'Title': 40, 'Author(s)': 25, 'Date': 18, 'Source': 30,
              'Sentence': 60, 'Paragraph': 80, 'PDF File': 20}
    for col_idx, col_name in enumerate(COLUMNS, start=1):
        col_letter = openpyxl.utils.get_column_letter(col_idx)
        ws.column_dimensions[col_letter].width = widths.get(col_name, 20)

    ws.freeze_panes = 'A2'
    ws.auto_filter.ref = ws.dimensions

    wb.save(output_path)
    print(f"\nSaved {len(rows)} rows to: {output_path}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description='Scrape pronoun occurrences from NewsBank PDF exports.'
    )
    parser.add_argument('pdfs', nargs='+', help='One or more PDF files to process')
    parser.add_argument('-o', '--output', default='pronoun_hits.xlsx',
                        help='Output spreadsheet filename (default: pronoun_hits.xlsx)')
    args = parser.parse_args()

    all_rows = []
    for pdf_path in args.pdfs:
        if not Path(pdf_path).exists():
            print(f"Warning: file not found: {pdf_path}")
            continue
        all_rows.extend(extract_pronoun_rows(pdf_path))

    if not all_rows:
        print("No pronoun occurrences found.")
        return

    write_xlsx(all_rows, args.output)


if __name__ == '__main__':
    main()
