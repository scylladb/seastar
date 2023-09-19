#!/bin/bash

# sets up the mounts needed for file_utils_test::test_detect_fs_type
# and runs the test, cleans up the mounts

set -euo pipefail

echo "BASE=${BASE:=$(mktemp --tmpdir -d fs_test.XXXXXX)}"
echo "BUILD_DIR=${BUILD_DIR=$(dirname "$0")/../../build/dev}"
echo "FS_SIZE=${FS_SIZE:=16M}" # ext3 filesystems smaller than about 8M tend to identify as ext2

fs_list=(ext2 ext3 ext4 xfs)

TEST_PATH="$BUILD_DIR/tests/unit/file_utils_test"
if [[ ! -e "$TEST_PATH" ]]; then
  echo "Test does not exist, did you build? Expected path: $TEST_PATH"
  exit 1
fi

unmount_all() {
  for fs in "${fs_list[@]}"; do
    if mountpoint "$BASE/mnt/$fs" > /dev/null; then
      sudo umount "$BASE/mnt/$fs"
    fi
  done
}

mkdir -p "$BASE/data" "$BASE/sym0" "$BASE/sym1"

# create and format the data files we'll use to loop-mount the file systems
for fs in "${fs_list[@]}"; do
  DF="$BASE/data/$fs.data"
  echo "Creating $fs file system inside $DF"
  truncate -s 0 "$DF"
  truncate -s "$FS_SIZE" "$DF"
  "mkfs.$fs" "$DF"
  mkdir -p "$BASE/mnt/$fs"
  sudo mount -t "$fs" -o loop "$DF" "$BASE/mnt/$fs"
  sudo chown "$USER:$USER" "$BASE/mnt/$fs"
  mkdir -p "$BASE/mnt/$fs/dir1/dir2"
  ln -s "$BASE/mnt/$fs" "$BASE/sym0/$fs" # symlink directly to mount
  ln -s "$BASE/mnt/$fs/dir1" "$BASE/sym1/$fs" # symlink to a dir inside mount
done

# run test
test_result=0
TEST_DETECT_FS_TYPE_ROOT=$BASE "$TEST_PATH" -t test_detect_fs_type --log_level=all || test_result=$?

# clean up
unmount_all
rm -rf "${BASE:?}/mnt" "${BASE:?}/data" "${BASE:?}/sym0" "${BASE:?}/sym1"
rmdir "$BASE"

echo "All mounts cleaned."

if [[ $test_result -eq 0 ]]; then
  echo "TEST PASSED"
else
  echo "TEST FAILED (rc=$test_result), see above"
fi






