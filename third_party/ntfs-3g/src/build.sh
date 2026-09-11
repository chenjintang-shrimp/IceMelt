#!/bin/bash

gcc ntfs-3g-fuse.c ntfs-3g-cli.c -o ntfs-3g-cli -I../include/ntfs-3g/ -L../libntfs-3g/.libs -lntfs-3g
