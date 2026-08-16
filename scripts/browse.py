#!/usr/bin/env python3

# init-generator.py (Systemd/OpenRC)

# Copyright 2023-2026 Pouria Rezaei <Pouria.rz@outlook.com>
# All rights reserved.
#
# Redistribution and use of this script, with or without modification, is
# permitted provided that the following conditions are met:
#
# 1. Redistributions of this script must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#
#  THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR IMPLIED
#  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
#  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
#  EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
#  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
#  OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
#  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
#  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
#  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import os
import platform
import subprocess
import time

def open_url(url: str) -> None:
    system = platform.system()

    if system == "Darwin":
        subprocess.run(["open", url], check=False)

    elif system == "Linux":
        subprocess.run(["xdg-open", url], check=False)

    elif system == "Windows":
        os.startfile(url)

    else:
        raise RuntimeError(f"Unsupported operating system: {system}")


def main() -> None:
    file_path = input("Enter the path to the text file containing URLs: ").strip()
    file_path = file_path.strip("\"'")

    try:
        with open(file_path, "r", encoding="utf-8") as file:
            urls = [line.strip() for line in file if line.strip()]
    except FileNotFoundError:
        print(f"Error: File not found: {file_path}")
        return
    except OSError as exc:
        print(f"Error reading file: {exc}")
        return

    if not urls:
        print("The file does not contain any URLs.")
        return

    print(f"Found {len(urls)} URL(s).")

    for url in urls:
        print(f"Opening: {url}")

        try:
            open_url(url)
        except (OSError, RuntimeError) as exc:
            print(f"Could not open {url}: {exc}")

        time.sleep(1)


if __name__ == "__main__":
    main()
