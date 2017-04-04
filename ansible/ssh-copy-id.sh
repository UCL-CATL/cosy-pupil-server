#!/bin/sh

ssh-copy-id root@130.104.191.153
ansible catl-test -a "cat ~/.ssh/authorized_keys" -u root
