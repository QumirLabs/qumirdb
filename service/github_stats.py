#!/usr/bin/env python3
"""Refresh the local GitHub statistics without putting API calls in browsers."""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import tempfile
import urllib.error
import urllib.request


REPOSITORY = 'QumirLabs/qumirdb'
REPO_URL = 'https://github.com/' + REPOSITORY


def github_json(endpoint):
    request = urllib.request.Request(
        'https://api.github.com/repos/' + REPOSITORY + endpoint,
        headers={'Accept': 'application/vnd.github+json', 'User-Agent': 'qumirdb-service'},
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)


def collect_stats():
    repo = github_json('')
    if not isinstance(repo, dict) or repo.get('full_name') != REPOSITORY:
        raise ValueError('Unexpected repository')
    for field in ('stargazers_count', 'forks_count'):
        if type(repo.get(field)) is not int or repo[field] < 0:
            raise ValueError('Invalid GitHub counter: ' + field)
    try:
        latest = github_json('/releases/latest')
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
        latest = None
    release = None
    if latest is not None:
        if not isinstance(latest, dict):
            raise ValueError('Invalid GitHub release')
        tag = latest.get('tag_name')
        url = latest.get('html_url')
        if not isinstance(tag, str) or not tag or not isinstance(url, str) or not url.startswith(REPO_URL + '/releases/tag/'):
            raise ValueError('Invalid GitHub release')
        release = {'tag': tag, 'url': url, 'published_at': latest.get('published_at')}
    return {
        'repository': REPOSITORY,
        'stars': repo['stargazers_count'],
        'forks': repo['forks_count'],
        'release': release,
        'updated_at': datetime.now(timezone.utc).isoformat(timespec='seconds'),
    }


def write_stats(output, stats):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=output.parent, delete=False) as stream:
            temporary = Path(stream.name)
            os.fchmod(stream.fileno(), 0o644)
            json.dump(stats, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
        os.replace(temporary, output)
    finally:
        if temporary is not None and temporary.exists():
            temporary.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        stats = collect_stats()
        write_stats(args.output, stats)
    except (OSError, ValueError) as error:
        parser.exit(1, f'github_stats: keeping previous statistics: {error}\n')
    print(f'github_stats: updated {args.output}')


if __name__ == '__main__':
    main()
