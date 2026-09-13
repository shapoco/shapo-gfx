#!/bin/bash

set -eux

python3 -m http.server --directory docs 52880
