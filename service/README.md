# QumirDB workbench service

## GitHub in the header

The header links to `QumirLabs/qumirdb`, shows stars and forks, and links to the
latest GitHub release. Icons and statistics are served locally. The repository
links also work without JavaScript; when statistics are unavailable the header
keeps the Star, Forks and Releases links.

Browsers load `github-stats.json` from the workbench. They do not contact the
GitHub API. The Debian service package depends on `cron` and installs
`/etc/cron.hourly/qumirdb-service-github-stats`, which refreshes the file every
hour. Updates are atomic; API or network failures preserve the previous
statistics. Before the first cron run, the UI uses the statistics shipped in
the package.

The cron job is registered as a Debian configuration file, so administrator
changes survive package upgrades. After package removal it exits without doing
anything because the updater is absent. Purging the package removes the cron
file as well.

To refresh the development copy:

```sh
python3 service/github_stats.py --output service/static/github-stats.json
```

On an installed system, run the hourly job manually to refresh immediately:

```sh
sudo /etc/cron.hourly/qumirdb-service-github-stats
```

The job suppresses successful refresh output; errors go to stderr for cron's
usual reporting.
