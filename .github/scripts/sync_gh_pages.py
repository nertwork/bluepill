#!/usr/bin/env python3
"""Synchronize documentation markdown files and assets to gh-pages."""

import os
import re
import shutil
import sys
import markdown


def get_version(repo_root: str) -> str:
    cmake_path = os.path.join(repo_root, "CMakeLists.txt")
    if os.path.isfile(cmake_path):
        with open(cmake_path, encoding="utf-8") as f:
            for line in f:
                m = re.search(r"project\s*\(\s*bluepill\s+VERSION\s+([0-9.]+)", line)
                if m:
                    return m.group(1)
    return "1.2.0"


def convert_and_embed_html(md_path: str, html_path: str) -> bool:
    if not os.path.isfile(md_path) or not os.path.isfile(html_path):
        return False
    with open(md_path, encoding="utf-8") as f:
        md_text = f.read()

    md = markdown.Markdown(extensions=["toc", "fenced_code", "tables"])
    body_html = md.convert(md_text)
    toc_html = md.toc

    with open(html_path, encoding="utf-8") as f:
        html_page = f.read()

    # Update TOC
    aside_regex = r"(<aside class=\"sidebar\">[\s\S]*?<div class=\"sidebar-title\">Table of Contents</div>\s*<div class=\"toc\">\s*)<div class=\"toc\">[\s\S]*?</div>(\s*</div>\s*</aside>)"
    if re.search(aside_regex, html_page):
        html_page = re.sub(aside_regex, lambda m: m.group(1) + toc_html + m.group(2), html_page)

    # Update body in <main class="doc-content">
    main_regex = r"(<main class=\"doc-content\">\s*<div class=\"doc-banner\">[\s\S]*?</div>\s*)<h1 id=[\s\S]*?(</main>)"
    if re.search(main_regex, html_page):
        html_page = re.sub(main_regex, lambda m: m.group(1) + body_html + "\n        " + m.group(2), html_page)

    with open(html_path, "w", encoding="utf-8") as f:
        f.write(html_page)
    return True


def main():
    if len(sys.argv) < 2:
        print("Usage: sync_gh_pages.py <gh-pages-worktree-dir>")
        sys.exit(1)

    target_dir = os.path.abspath(sys.argv[1])
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

    version = get_version(repo_root)
    print(f"Syncing documentation for BluePill v{version} to {target_dir}...")

    # Remove stray tests directory if present
    stray_tests = os.path.join(target_dir, "tests")
    if os.path.isdir(stray_tests):
        shutil.rmtree(stray_tests, ignore_errors=True)
        print("Pruned stray tests/ from gh-pages.")

    # Files to sync: (source_rel, dest_filename, html_filename)
    sync_map = [
        ("docs/AGENT_INSTALL_PLAYBOOK.md", "AGENT_INSTALL_PLAYBOOK.md", "AGENT_INSTALL_PLAYBOOK.html"),
        ("docs/ARCHITECTURE.md", "ARCHITECTURE.md", "ARCHITECTURE.html"),
        ("docs/BLUEBUBBLES_SETUP_GUIDE.md", "BLUEBUBBLES_SETUP_GUIDE.md", "BLUEBUBBLES_SETUP_GUIDE.html"),
        ("docs/CONFIGURATION_AND_CLI.md", "CONFIGURATION_AND_CLI.md", "CONFIGURATION_AND_CLI.html"),
        ("docs/INSTALLATION_AND_OPERATIONS_GUIDE.md", "INSTALLATION_AND_OPERATIONS_GUIDE.md", "INSTALLATION_AND_OPERATIONS_GUIDE.html"),
        ("AGENTS.md", "AGENTS.md", "AGENTS.html"),
    ]

    for src_rel, dest_md, dest_html in sync_map:
        src_full = os.path.join(repo_root, src_rel)
        dest_md_full = os.path.join(target_dir, dest_md)
        dest_html_full = os.path.join(target_dir, dest_html)

        if os.path.isfile(src_full):
            shutil.copyfile(src_full, dest_md_full)
            if os.path.isfile(dest_html_full):
                convert_and_embed_html(dest_md_full, dest_html_full)
                print(f"Synced & converted {dest_md} -> {dest_html}")
            else:
                print(f"Synced {dest_md}")

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

    # Update version in target index.html and doc headers
    idx_path = os.path.join(target_dir, "index.html")
    if os.path.isfile(idx_path):
        with open(idx_path, encoding="utf-8") as f:
            idx_text = f.read()
        idx_text = re.sub(r"BluePill v[0-9.]+", f"BluePill v{version}", idx_text)
        with open(idx_path, "w", encoding="utf-8") as f:
            f.write(idx_text)
        print("Updated version in index.html")

    print("Documentation sync complete.")


if __name__ == "__main__":
    main()
