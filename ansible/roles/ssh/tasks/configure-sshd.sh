#!/usr/bin/env bash

# Disable ssh password authentication. Must authenticate with an authorized key.
sed -i 's/^PasswordAuthentication yes$/PasswordAuthentication no/' /etc/ssh/sshd_config
