#!/bin/env python3

import subprocess, os, sys, re, shutil, io, posixpath
import argparse, markdown
from urllib.parse import urlsplit, urlunsplit, unquote, quote
from markdown.extensions import Extension
from markdown.extensions.toc import TocExtension
from markdown.treeprocessors import Treeprocessor
from bs4 import BeautifulSoup as bs, NavigableString
import datetime as dt

def hhdr(name='', title=None, extra=''):
    if not title: title = f'antcc doc: {name}' if name != 'index' else 'antcc'
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>{title}</title>
  <link rel='shortcut icon' href='favicon.ico'/>
  <link rel="stylesheet" href="style.css"/>
</head>
<body><div id="content">{extra}
"""

def hftr(name='', meta=None):
    if not meta: meta = f'generated from {name}'
    date = dt.date.today().isoformat()
    return f"""</div>
<table class="mainfoot">
  <tbody><tr>
    <td class="mainfoot">{meta}</td>
    <td class="mainfoot">{date}</td>
  </tr>
</tbody></table>
</body></html>
"""

NAV="""
<nav class="navbar">
<a href=".">/antcc</a>
</nav><hr>
"""

# rewrite relative links to git repo
def rewrite_relative(href):
    REPO_BASE = 'https://git.386s.org/antcc/'
    FILE_DIR = 'doc/'
    if not href or href.startswith(("/", "#", "?")):
        return href
    parts = urlsplit(href)
    target = posixpath.normpath(posixpath.join(FILE_DIR, unquote(parts.path)))
    if parts.scheme or parts.netloc or not parts.path or target == '.':
        return href
    ext = posixpath.splitext(target)[1].lower()
    if target.startswith(FILE_DIR) and ext == '.html':
        return href
    if ext in (".md", ".txt"):
        route = "about/"
    else:
        route = "tree/"
    return urlunsplit(("", "", REPO_BASE + route + quote(target),
               parts.query, parts.fragment))

class RelativeLinks(Extension):
    def extendMarkdown(self, md):
        class Pp(Treeprocessor):
            def run(self, root):
                for anchor in root.iter("a"):
                    href = anchor.get("href")
                    rewritten = rewrite_relative(href)
                    if rewritten != href:
                        anchor.set("href", rewritten)
                return root
        md.treeprocessors.register(Pp(md.parser), 'relative-links', 5)

def exc(cmd, input=None):
    p = subprocess.run(cmd, capture_output=True, input=input.encode('utf-8') if input else None) 
    return p.stdout.decode('utf-8')

def gen(src, to):
    ft = re.search(r'\..+$', src)[0]
    if ft == '.md':
        print(f'markdown {src} -> {to}')
        name = re.sub(r'\..+$', '', src).split('/')[-1]
        md = io.BytesIO()
        toc = name != 'index'
        markdown.markdownFromFile(input=src, output=md, encoding='utf-8',
                                  extensions = ["markdown.extensions.fenced_code",
                                                "markdown.extensions.codehilite",
                                                "markdown.extensions.tables",
                                                "markdown.extensions.sane_lists",
                                                TocExtension(anchorlink=True) if toc else None,
                                                RelativeLinks()])
        html = hhdr(name, extra=NAV if name != 'index' else '') + md.getvalue().decode('utf-8') + hftr(src)
        with open(to, 'w') as f:
            f.write(html)
    elif ft == '.html' or ft == '.css':
        print(f'copy {src} -> {to}')
        shutil.copy(src, to)

def mandoc_clean(html):
    html = re.sub(r'(\<b\> \<\/b\>) (\[options\])', r'<span class="tb">\2</span>', html)
    soup = bs(html, 'html.parser')
    # drop -option links
    for dt in soup.find_all("dt"):
        for a in dt.find_all("a"):
            a.unwrap()

    # xrefs
    SEC = re.compile(r"\((\d\w*)\)")
    URL = "https://linux.die.net/man/%s/%s"
    for b in soup.find_all("b"):
        nxt = b.next_sibling
        if not (isinstance(nxt, NavigableString) and (m := SEC.match(nxt))):
            continue

        section, rest = m.group(1), nxt[m.end():]
        name = b.get_text(strip=True)
        a = soup.new_tag("a", href=URL % (section, name))
        a.string = f"{name}({section})"

        b.replace_with(a)
        nxt.replace_with(NavigableString(rest))
    return str(soup)

def genman(roff, to):
    print(f'man -> {to}')
    html = exc(['mandoc', '-man', '-Thtml', '-O','fragment', '-Kutf-8'], input=roff)
    html = mandoc_clean(html)
    html = hhdr(title='antcc(1)') + '<div class="man">'+html+'</div>' + hftr("antcc --man")
    with open(to, 'w') as f:
        f.write(html)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    gx = ap.add_mutually_exclusive_group()
    gx.add_argument('-d', default="doc/", help="path to docs directory")
    ap.add_argument('-a', '--antcc', default="./antcc", help="path to antcc executable")
    ap.add_argument("outdir", help="directory in which to put generated HTML")
    args = ap.parse_args()
    outdir = args.outdir
    docfiles = os.listdir(args.d)
    roff = exc([args.antcc, "--man"])
    os.makedirs(outdir, exist_ok=True)
    for f in docfiles:
        gen('doc/'+f, outdir+'/'+re.sub(r'\.(md|txt)+$', '.html', f))
    genman(roff, outdir+'/antcc.1.html')
