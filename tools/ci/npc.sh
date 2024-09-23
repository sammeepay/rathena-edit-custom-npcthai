#!/usr/bin/env bash

out=npcTH/scripts_custom.conf

printf "\n" >> $out
echo "// Custom Scripts" >> $out

find npcTH/custom \( -name "*.txt" \) | sort | xargs -I % echo "npc: %" >> $out

echo "// Test Scripts" >> $out

find npcTH/test \( -name "*.txt" \) | sort | xargs -I % echo "npc: %" >> $out
