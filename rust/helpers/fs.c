// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (C) 2024 Google LLC.
 */

#include <linux/fs.h>

__rust_helper struct file *rust_helper_get_file(struct file *f)
{
	return get_file(f);
}

const struct path *rust_helper_file_user_path(const struct file *f)
{
	return file_user_path(f);
}
