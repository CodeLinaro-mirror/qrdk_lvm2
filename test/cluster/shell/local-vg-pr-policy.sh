#!/bin/bash
# persistent_reservations policy values in lvm.conf
# (see PR_POLICY_REDESIGN_20261112.md)
#
# - the setting is a string list in which a single value is currently
#   used: auto (the default), full or none
# - invalid values are rejected at command start (unknown value,
#   including the old opt-in/never/require values, and more than one
#   value in the list)
# - full without a pr_key or host_id fails fast; auto and none
#   do not (auto uses PR automatically only with lvmlockd GM2, which
#   needs only the sanlock host_id)
# - full: vgcreate enables PR on the new VG by default (the same as
#   --setpersist y); an explicit --setpersist n wins over the default
# - full with a dev that does not support PR: the VG is created
#   without PR (fallback; covered further by
#   local-vg-global-devices-pr.sh)
# - vgcreate --pr is a shortcut for --setpersist y --persist start:
#   the new VG has PR enabled and started; it cannot be combined with
#   --setpersist or --persist
# - none forbids PR on the host: vgchange --persist start, vgchange
#   --setpersist y and vgcreate --setpersist fail; a VG requiring PR
#   explains the conflict; --setpersist n remains allowed (the remedy)

set -e

if [ "$CLUSTER_NUM_NODES" -lt 2 ]; then
    echo "SKIP: This test requires at least 2 nodes (found $CLUSTER_NUM_NODES)"
    exit 0
fi

# set_pr <value>: set global/persistent_reservations in lvm.conf (the
# executor's lvm.conf has no persistent_reservations line; the first call
# appends it, later calls replace the value)
set_pr() {
    if node1 "grep -q persistent_reservations /etc/lvm/lvm.conf"; then
        node1 "sed -i -e 's/persistent_reservations = .*/persistent_reservations = $1/' /etc/lvm/lvm.conf"
    else
        node1 "printf 'global {\n\tpersistent_reservations = $1\n}\n' >> /etc/lvm/lvm.conf"
    fi
    node1 "lvmconfig --mergedconfig global/persistent_reservations | grep -q 'persistent_reservations=\"$1\"'"
}

node1 'sed -i -e "/pr_key/d" -e "/host_id/d" -e "/local {/a\\    pr_key = \"0xaa11\"" /etc/lvm/lvmlocal.conf'
node1 "lvmconfig local/pr_key | grep -q 'pr_key=\"0xaa11\"'"

# clear leftover labels
node1 "dd if=/dev/zero of=$dev1 bs=1M count=2"
node1 "dd if=/dev/zero of=$dev2 bs=1M count=2"

echo "== Test 1: invalid values are rejected =="

# the default is auto
node1 "lvmconfig --type default global/persistent_reservations | grep -q 'persistent_reservations=\[\"auto\"\]'"

node1 "not vgs --config 'global { persistent_reservations = \"bogus\" }' 2>&1" > out
grep -q "Unknown persistent_reservations value: bogus" out

# the old policy values are no longer accepted
node1 "not vgs --config 'global { persistent_reservations = \"opt-in\" }' 2>&1" > out
grep -q "Unknown persistent_reservations value: opt-in" out

node1 "not vgs --config 'global { persistent_reservations = \"never\" }' 2>&1" > out
grep -q "Unknown persistent_reservations value: never" out

node1 "not vgs --config 'global { persistent_reservations = \"require\" }' 2>&1" > out
grep -q "Unknown persistent_reservations value: require" out

# a single value may be given as a plain string or a one-element list
node1 "vgs --config 'global { persistent_reservations = [\"full\"] }' 2>&1" > out

# more than one value in the list is rejected
node1 "not vgs --config 'global { persistent_reservations = [\"auto\", \"full\"] }' 2>&1" > out
grep -q "Invalid persistent_reservations value: .full. conflicts with an earlier setting" out

echo "== Test 2: full without a pr_key or host_id fails fast =="

# The executor flattens lvmlocal.conf into lvm.conf at test setup, so
# stale keys can survive in lvm.conf after they are removed from
# lvmlocal.conf; remove them from both files for this check.
node1 'sed -i -e "/pr_key/d" -e "/host_id/d" /etc/lvm/lvmlocal.conf'
node1 'sed -i -e "/pr_key/d" -e "/host_id/d" /etc/lvm/lvm.conf'

node1 "not vgs --config 'global { persistent_reservations = \"full\" }' 2>&1" > out
grep -q "A local pr_key or host_id is required to use persistent reservations" out

# auto and none need no pr_key or host_id: auto uses PR
# automatically only with lvmlockd GM2 (which needs only the sanlock
# host_id), and none uses no PR at all
node1 "vgs --config 'global { persistent_reservations = \"auto\" }' 2>&1" > out
node1 "vgs --config 'global { persistent_reservations = \"none\" }' 2>&1" > out

node1 'sed -i -e "/local {/a\\    pr_key = \"0xaa11\"" /etc/lvm/lvmlocal.conf'
node1 "lvmconfig local/pr_key | grep -q 'pr_key=\"0xaa11\"'"

echo "== Test 3: full enables PR on new VGs by default =="

set_pr full

# as with an explicit --setpersist y (without --persist start), the new VG
# requires PR to be started before it can be used
node1 "vgcreate testvg $dev1 2>&1" > out
node1 "vgs --noheadings -o persist testvg" > out
grep -q "require,autostart" out

node1 "not lvcreate -y -l1 testvg 2>&1" > out
grep -q "Cannot access VG testvg without persistent reservation" out

node1 "vgchange --persist start testvg 2>&1" > out
grep -q "started.*with key 0xaa11" out
node1 "lvcreate -y -l1 -n lv1 testvg"
node1 "lvmpersist check-key --key 0xaa11 --device $dev1"

node1 "vgremove -y testvg 2>&1" > out
grep -q "stopped.*with key 0xaa11" out
node1 "lvmpersist read-keys --device $dev1 | grep -q 'keys: none'"

# an explicit --setpersist n wins over the policy default
node1 "vgcreate --setpersist n testvg $dev1 2>&1" > out
node1 "vgs --noheadings -o persist testvg" > out
if grep -q "require" out; then
    echo "ERROR: --setpersist n did not override full"
    exit 1
fi
node1 vgremove -y testvg

echo "== Test 4: full with a non-PR dev falls back =="

# clean up any loop device left over from a previous (failed) run
node1 "losetup -j /tmp/t8-nonpr.img -O NAME | xargs -r losetup -d; rm -f /tmp/t8-nonpr.img"

# a loop device does not support PR
node1 "truncate -s 128m /tmp/t8-nonpr.img"
LOOP=$(node1 "losetup -f --show /tmp/t8-nonpr.img" | tail -1)
node1 udevadm settle

# full falls back: the VG is created without PR
node1 "vgcreate testvg $LOOP 2>&1" > out
node1 "vgs --noheadings -o persist testvg" > out
if grep -q "require" out; then
    echo "ERROR: full did not fall back on a non-PR dev"
    exit 1
fi
node1 vgremove -y testvg

node1 "losetup -d $LOOP"
node1 "rm -f /tmp/t8-nonpr.img"

echo "== Test 5: --pr is a shortcut for --setpersist y --persist start =="

set_pr auto

# the shortcut cannot be combined with the options it stands for
node1 "not vgcreate --pr --setpersist y testvg $dev1 2>&1" > out
grep -q "Command does not accept option combination: --pr.*with --setpersist" out

node1 "not vgcreate --pr --persist start testvg $dev1 2>&1" > out
grep -q "Command does not accept option combination: --pr.*with --persist" out

# the new VG has PR enabled and started, as with
# --setpersist y --persist start
node1 "vgcreate --pr testvg $dev1 2>&1" > out
node1 "vgs --noheadings -o persist testvg" > out
grep -q "require,autostart" out

# PR is started: the key is registered and the VG can be used
node1 "lvmpersist check-key --key 0xaa11 --device $dev1"
node1 "lvcreate -y -l1 -n lv1 testvg"

node1 "vgremove -y testvg 2>&1" > out
grep -q "stopped.*with key 0xaa11" out
node1 "lvmpersist read-keys --device $dev1 | grep -q 'keys: none'"

node1 "dd if=/dev/zero of=$dev1 bs=1M count=2"

echo "== Test 6: none forbids PR on the host =="

# a VG with require set and PR not started (created under the default
# auto policy with an explicit --setpersist y)
set_pr auto
node1 "vgcreate --setpersist y testvg $dev2 2>&1" > out
node1 "vgs --noheadings -o persist testvg" > out
grep -q "require,autostart" out

set_pr none

node1 "not vgchange --persist start testvg 2>&1" > out
grep -q "not permitted on this host (persistent_reservations=none)" out

node1 "not vgchange --setpersist y testvg 2>&1" > out
grep -q "not permitted on this host (persistent_reservations=none)" out

node1 "not vgcreate --setpersist y othervg $dev1 2>&1" > out
grep -q "not permitted on this host (persistent_reservations=none)" out

node1 "not vgcreate --pr othervg $dev1 2>&1" > out
grep -q "not permitted on this host (persistent_reservations=none)" out

# a VG requiring PR explains the host-policy conflict
node1 "not lvcreate -y -l1 testvg 2>&1" > out
grep -q "requires persistent reservations, but PR is not permitted on this host" out

# a plain vgcreate (no PR options) is unaffected
node1 "vgcreate othervg $dev1 2>&1" > out
node1 vgremove -y othervg

# --setpersist n remains allowed: the remedy for a require VG on a none host
node1 "vgchange --setpersist n testvg 2>&1" > out
node1 "lvcreate -y -l1 -n lv1 testvg"
node1 vgremove -y testvg

node1 "dd if=/dev/zero of=$dev1 bs=1M count=2"
node1 "dd if=/dev/zero of=$dev2 bs=1M count=2"

echo "== Test passed =="
