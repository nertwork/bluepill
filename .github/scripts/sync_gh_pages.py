#!/usr/bin/env python3
"""Synchronize documentation markdown files and assets to gh-pages (or regenerate docs/*.html on main)."""

import argparse
import os
import re
import shutil
import sys
import markdown


SYNC_MAP = [
    ("docs/index.html", "index.html", None),
    ("docs/AGENT_INSTALL_PLAYBOOK.md", "AGENT_INSTALL_PLAYBOOK.md", "AGENT_INSTALL_PLAYBOOK.html"),
    ("docs/ARCHITECTURE.md", "ARCHITECTURE.md", "ARCHITECTURE.html"),
    ("docs/BLUEBUBBLES_SETUP_GUIDE.md", "BLUEBUBBLES_SETUP_GUIDE.md", "BLUEBUBBLES_SETUP_GUIDE.html"),
    ("docs/CONFIGURATION_AND_CLI.md", "CONFIGURATION_AND_CLI.md", "CONFIGURATION_AND_CLI.html"),
    ("docs/INSTALLATION_AND_OPERATIONS_GUIDE.md", "INSTALLATION_AND_OPERATIONS_GUIDE.md", "INSTALLATION_AND_OPERATIONS_GUIDE.html"),
    ("AGENTS.md", "AGENTS.md", "AGENTS.html"),
]

STATIC_DOCS_FILES = [
    "404.html",
    "_config.yml",
    "ef0a8b4c9d1e2f3a4b5c6d7e8f9a0b1c.txt",
    "logo.png",
    "robots.txt",
    "screenshot.png",
    "sitemap.xml",
]


def get_version(repo_root: str) -> str:
    cmake_path = os.path.join(repo_root, "CMakeLists.txt")
    if os.path.isfile(cmake_path):
        with open(cmake_path, encoding="utf-8") as f:
            for line in f:
                m = re.search(r"project\s*\(\s*bluepill\s+VERSION\s+([0-9.]+)", line)
                if m:
                    return m.group(1)
    return "1.2.12"


def rewrite_doc_links_for_pages(html_text: str) -> str:
    """Convert internal markdown doc links into .html links for GitHub Pages."""
    html_text = re.sub(
        r'href="(?:docs/)?([A-Z0-9_]+)\.md(#[^"]*)?"',
        lambda m: f'href="{m.group(1)}.html{m.group(2) or ""}"',
        html_text,
    )
    return html_text


def update_index_versions(idx_text: str, version: str) -> str:
    idx_text = re.sub(r"BluePill v[0-9.]+", f"BluePill v{version}", idx_text)
    idx_text = re.sub(
        r'"softwareVersion":\s*"[0-9.]+"',
        f'"softwareVersion": "{version}"',
        idx_text,
    )
    idx_text = re.sub(
        r"bluepill_[0-9.]+_amd64\.deb",
        f"bluepill_{version}_amd64.deb",
        idx_text,
    )
    idx_text = re.sub(
        r"bluepill-[0-9.]+-1(?:\.fc[0-9]+)?\.x86_64\.rpm",
        f"bluepill-{version}-1.x86_64.rpm",
        idx_text,
    )
    idx_text = re.sub(
        r"BluePill-Setup-[0-9.]+-win64\.exe",
        f"BluePill-Setup-{version}-win64.exe",
        idx_text,
    )
    canonical_root = '<link rel="canonical" href="https://bluepill.app/">'
    if re.search(r'<link\s+rel="canonical"[^>]*>', idx_text, flags=re.IGNORECASE):
        idx_text = re.sub(
            r'<link\s+rel="canonical"[^>]*>',
            canonical_root,
            idx_text,
            count=1,
            flags=re.IGNORECASE,
        )
    elif "</head>" in idx_text:
        idx_text = idx_text.replace("</head>", f"    {canonical_root}\n</head>", 1)
    return idx_text


def convert_and_embed_html(md_path: str, html_path: str, version: str) -> bool:
    if not os.path.isfile(md_path) or not os.path.isfile(html_path):
        return False
    with open(md_path, encoding="utf-8") as f:
        md_text = f.read()

    md_text = re.sub(r"\A---\r?\n[\s\S]*?\r?\n---\r?\n*", "", md_text)

    md = markdown.Markdown(extensions=["toc", "fenced_code", "tables"])
    body_html = rewrite_doc_links_for_pages(md.convert(md_text))
    toc_html = md.toc

    with open(html_path, encoding="utf-8") as f:
        html_page = f.read()

    # Resolve __TITLE__ metadata placeholder from <title>
    title_match = re.search(r"<title>(.*?)</title>", html_page, flags=re.IGNORECASE | re.DOTALL)
    if title_match:
        page_title = title_match.group(1).split(" — ")[0].strip()
        html_page = html_page.replace("__TITLE__", page_title)

    # Update TOC
    aside_regex = (
        r'(<aside class="sidebar">[\s\S]*?<div class="sidebar-title">Table of Contents</div>\s*'
        r'<div class="toc">\s*)[\s\S]*?(\s*</div>\s*</aside>)'
    )
    if re.search(aside_regex, html_page):
        html_page = re.sub(aside_regex, lambda m: m.group(1) + toc_html + m.group(2), html_page)

    # Rebuild <main class="doc-content"> cleanly with an intact .doc-banner and updated version
    banner_html = (
        '<div class="doc-banner">\n'
        '                <div>\n'
        f'                    <strong>💊 BluePill v{version} Documentation Portal</strong>\n'
        '                    <div style="font-size: 0.85rem; color: var(--text-muted); margin-top: 2px;">\n'
        '                        Interactive guide with verified copy-paste commands and architecture reference.\n'
        '                    </div>\n'
        '                </div>\n'
        '                <a href="index.html">← Home</a>\n'
        '            </div>\n\n            '
    )
    main_regex = r'(<main class="doc-content">\s*)[\s\S]*?(</main>)'
    if re.search(main_regex, html_page):
        html_page = re.sub(
            main_regex,
            lambda m: m.group(1) + banner_html + body_html + "\n        " + m.group(2),
            html_page,
        )

    # Consolidate search ranking onto official bluepill.app/docs
    canonical_tag = '<link rel="canonical" href="https://bluepill.app/docs">'
    if re.search(r'<link\s+rel="canonical"[^>]*>', html_page, flags=re.IGNORECASE):
        html_page = re.sub(
            r'<link\s+rel="canonical"[^>]*>',
            canonical_tag,
            html_page,
            count=1,
            flags=re.IGNORECASE,
        )
    elif "</head>" in html_page:
        html_page = html_page.replace("</head>", f"    {canonical_tag}\n</head>", 1)

    with open(html_path, "w", encoding="utf-8") as f:
        f.write(html_page)
    return True


def sync_main_html(repo_root: str, version: str) -> None:
    docs_dir = os.path.join(repo_root, "docs")
    print(f"Regenerating checked-in docs/*.html for BluePill v{version} in {docs_dir}...")
    idx_path = os.path.join(docs_dir, "index.html")
    if os.path.isfile(idx_path):
        with open(idx_path, encoding="utf-8") as f:
            idx_text = f.read()
        with open(idx_path, "w", encoding="utf-8") as f:
            f.write(update_index_versions(idx_text, version))
        print("Updated docs/index.html")

    for src_rel, _, dest_html in SYNC_MAP:
        if not dest_html:
            continue
        src_full = os.path.join(repo_root, src_rel)
        html_full = os.path.join(docs_dir, dest_html)
        if convert_and_embed_html(src_full, html_full, version):
            print(f"Regenerated docs/{dest_html} from {src_rel}")


def sync_gh_pages(repo_root: str, target_dir: str, version: str) -> None:
    print(f"Syncing documentation for BluePill v{version} to {target_dir}...")

    # Remove stray tests directory if present
    stray_tests = os.path.join(target_dir, "tests")
    if os.path.isdir(stray_tests):
        shutil.rmtree(stray_tests, ignore_errors=True)
        print("Pruned stray tests/ from gh-pages.")

    for src_rel, dest_md, dest_html in SYNC_MAP:
        src_full = os.path.join(repo_root, src_rel)
        dest_md_full = os.path.join(target_dir, dest_md)

        if os.path.isfile(src_full):
            if src_rel == "AGENTS.md":
                with open(src_full, encoding="utf-8") as f:
                    agents_txt = f.read()
                agents_txt = agents_txt.replace("(docs/", "(")
                with open(dest_md_full, "w", encoding="utf-8") as f:
                    f.write(agents_txt)
            else:
                shutil.copyfile(src_full, dest_md_full)

            if dest_html:
                dest_html_full = os.path.join(target_dir, dest_html)
                if not os.path.isfile(dest_html_full):
                    repo_html_full = os.path.join(repo_root, "docs", dest_html)
                    if os.path.isfile(repo_html_full):
                        shutil.copyfile(repo_html_full, dest_html_full)
                if os.path.isfile(dest_html_full):
                    convert_and_embed_html(dest_md_full, dest_html_full, version)
                    print(f"Synced & converted {dest_md} -> {dest_html}")
                else:
                    print(f"Synced {dest_md}")
            else:
                print(f"Synced {dest_md}")

    for static_name in STATIC_DOCS_FILES:
        src_static = os.path.join(repo_root, "docs", static_name)
        if os.path.isfile(src_static):
            shutil.copyfile(src_static, os.path.join(target_dir, static_name))

    # Copy screenshots if present
    src_screenshots = os.path.join(repo_root, "docs", "screenshots")
    if os.path.isdir(src_screenshots):
        dest_screenshots = os.path.join(target_dir, "screenshots")
        os.makedirs(dest_screenshots, exist_ok=True)
        for item in os.listdir(src_screenshots):
            s = os.path.join(src_screenshots, item)
            d = os.path.join(dest_screenshots, item)
            if os.path.isfile(s):
                shutil.copyfile(s, d)
        print("Synced screenshots/")

    # Update version in target index.html
    idx_path = os.path.join(target_dir, "index.html")
    if os.path.isfile(idx_path):
        with open(idx_path, encoding="utf-8") as f:
            idx_text = f.read()
        with open(idx_path, "w", encoding="utf-8") as f:
            f.write(update_index_versions(idx_text, version))
        print("Updated version and canonical link in index.html")


def main():
    parser = argparse.ArgumentParser(description="Sync BluePill documentation to gh-pages or docs/*.html")
    parser.add_argument("target_dir", nargs="?", help="gh-pages worktree directory")
    parser.add_argument("--sync-main-html", action="store_true", help="Regenerate checked-in docs/*.html in main")
    args = parser.parse_args()

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    version = get_version(repo_root)

    if args.sync_main_html:
        sync_main_html(repo_root, version)
    elif args.target_dir:
        sync_gh_pages(repo_root, os.path.abspath(args.target_dir), version)
    else:
        parser.print_help()
        sys.exit(1)

    print("Documentation sync complete.")


if __name__ == "__main__":
    main()
