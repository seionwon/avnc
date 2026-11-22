"""Publish the signed installer already checked in on the fork's default branch."""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from zipfile import ZipFile


REPOSITORY = "seionwon/avnc"
API = f"repos/{REPOSITORY}/releases"


def api(path, *, method=None, body=None, paginate=False):
    command = ["gh", "api", path]
    if paginate:
        command.extend(["--paginate", "--slurp"])
    if method:
        command.extend(["--method", method])
    if body is not None:
        command.extend(["--input", "-"])
    response = subprocess.check_output(
        command, input=json.dumps(body).encode() if body is not None else None
    )
    return json.loads(response) if response.strip() else None


def git(*arguments):
    return subprocess.check_output(["git", *arguments], text=True).strip()


def verify_asset(asset, apk, digest):
    if asset["state"] != "uploaded" or asset["size"] != apk.stat().st_size:
        raise SystemExit(f"Incomplete release asset: {apk.name}")
    remote_digest = asset.get("digest")
    if not remote_digest:
        with tempfile.TemporaryFile() as downloaded:
            subprocess.run(
                ["gh", "api", f"{API}/assets/{asset['id']}", "-H", "Accept: application/octet-stream"],
                stdout=downloaded, check=True,
            )
            downloaded.seek(0)
            remote_digest = "sha256:" + hashlib.file_digest(downloaded, "sha256").hexdigest()
    if remote_digest != digest:
        raise SystemExit(f"A different release asset already uses the name {apk.name}")


def main():
    if os.environ.get("GITHUB_REPOSITORY", "").casefold() != REPOSITORY or os.environ.get("GITHUB_REF") != "refs/heads/master":
        raise SystemExit("Publish from the master branch of seionwon/avnc.")
    gradle = Path("app/build.gradle.kts").read_text()
    match = re.search(r'^\s*versionName\s*=\s*"([\w.+-]+-seionwon)"\s*$', gradle, re.MULTILINE)
    if not match:
        raise SystemExit("The Android build must specify the fork versionName.")
    version = match[1]
    tag = f"v{version}"
    apk = Path("downloads") / f"AVNC-{tag}.apk"
    with ZipFile(apk) as archive:
        if "AndroidManifest.xml" not in archive.namelist() or "classes.dex" not in archive.namelist() or archive.testzip():
            raise SystemExit(f"Invalid Android APK: {apk}")
    with apk.open("rb") as contents:
        digest = "sha256:" + hashlib.file_digest(contents, "sha256").hexdigest()

    homepage = Path(".github/README.md").read_text()
    _, start, section = homepage.partition("<!-- release-notes:start -->")
    notes, end, _ = section.partition("<!-- release-notes:end -->")
    raw_url = f"https://github.com/{REPOSITORY}/raw/refs/heads/master/{apk.as_posix()}"
    if not start or not end or raw_url not in notes:
        raise SystemExit("The homepage release section must link to the current APK.")
    asset_url = f"https://github.com/{REPOSITORY}/releases/download/{tag}/{apk.name}"
    notes = notes.strip().replace(raw_url, asset_url)
    title = f"AVNC {version}"
    commit = git("rev-parse", "HEAD")
    blob = git("hash-object", str(apk))
    if git("rev-parse", f"HEAD:{apk.as_posix()}") != blob:
        raise SystemExit("Commit the installer before publishing the release.")

    pages = api(f"{API}?per_page=100", paginate=True)
    matches = [item for page in pages for item in page if item["tag_name"] == tag]
    if len(matches) > 1:
        raise SystemExit(f"Multiple releases use {tag}. Check the drafts before publishing.")
    release = matches[0] if matches else None
    if release and release["draft"]:
        if (release["name"] != title or (release["body"] or "").replace("\r\n", "\n").strip() != notes
                or any(item["name"] != apk.name for item in release["assets"])):
            raise SystemExit(f"An existing draft for {tag} has different contents.")

    assets = release["assets"] if release else []
    asset = next((item for item in assets if item["name"] == apk.name), None)
    if not asset or release["draft"]:
        refs = [f"refs/tags/{name}" for name in git("tag", "--list", tag).splitlines()]
        if release and release["draft"]:
            refs.append(release["target_commitish"])
        for ref in refs:
            if git("rev-parse", "--verify", "--end-of-options", f"{ref}:{apk.as_posix()}") != blob:
                raise SystemExit(f"{ref} points to another installer. Check the release version.")
    if not release:
        release = api(API, method="POST", body={
            "tag_name": tag, "name": title, "target_commitish": commit, "body": notes, "draft": True,
        })
    endpoint = f"{API}/{release['id']}"
    if release["draft"] and asset and asset["state"] == "starter" and asset["size"] == 0:
        api(f"{API}/assets/{asset['id']}", method="DELETE")
        asset = None
    if not asset:
        subprocess.run(["gh", "release", "upload", tag, str(apk), "--repo", REPOSITORY], check=True)
        release = api(endpoint)
        asset = next(item for item in release["assets"] if item["name"] == apk.name)
    verify_asset(asset, apk, digest)
    if release["draft"]:
        release = api(endpoint, method="PATCH", body={"draft": False, "make_latest": "legacy"})
    if release["draft"]:
        raise SystemExit(f"{tag} is still a draft.")
    print(release["html_url"])


if __name__ == "__main__":
    main()
