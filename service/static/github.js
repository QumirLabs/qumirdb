const repository = 'QumirLabs/qumirdb';
const repoUrl = 'https://github.com/' + repository;

export async function initGitHubProject() {
  const project = document.getElementById('github-project');
  if (!project) {
    return;
  }

  try {
    const response = await fetch(new URL('./github-stats.json', import.meta.url), {
      cache: 'no-cache',
      signal: AbortSignal.timeout(5000),
    });
    if (!response.ok) {
      return;
    }
    const stats = await response.json();
    if (!stats || stats.repository !== repository
        || !Number.isSafeInteger(stats.stars) || stats.stars < 0
        || !Number.isSafeInteger(stats.forks) || stats.forks < 0) {
      return;
    }

    const count = new Intl.NumberFormat('en', {
      notation: 'compact', maximumFractionDigits: 1,
    });
    project.querySelector('[data-github-stars]').textContent = count.format(stats.stars);
    project.querySelector('[data-github-forks]').textContent = count.format(stats.forks);
    document.getElementById('github-stars').title =
      `Enjoying QumirDB? Give it a star on GitHub (${stats.stars})`;
    document.getElementById('github-forks').title = `QumirDB forks on GitHub (${stats.forks})`;

    const release = document.getElementById('github-release');
    if (stats.release && typeof stats.release.tag === 'string' && stats.release.tag
        && typeof stats.release.url === 'string'
        && stats.release.url.startsWith(repoUrl + '/releases/tag/')) {
      release.textContent = stats.release.tag;
      release.href = stats.release.url;
      release.title = 'Latest QumirDB release: ' + stats.release.tag;
    } else if (stats.release === null) {
      release.textContent = 'Releases';
      release.href = repoUrl + '/releases';
      release.title = 'QumirDB releases';
    }
  } catch {
    // Keep the links usable when the statistics refresh is unavailable.
  }
}
