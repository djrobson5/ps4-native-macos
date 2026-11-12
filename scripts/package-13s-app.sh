#!/usr/bin/env bash
# scripts/package-13s-app.sh: builds "13 Sentinels.app" and puts it, with a copy of the game's
# folder, in ~/Games/13 Sentinels Aegis Rim. The launcher calls this one, so its options stay as they
# were (--dest, --game, --icon, --no-game, --icon-cache); scripts/package-app.sh --title 13s does
# the work and documents them.
exec "$(dirname -- "$0")/package-app.sh" --title 13s "$@"
