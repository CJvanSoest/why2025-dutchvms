/* This file is part of BadgeVMS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "socket.h"

#include "esp_log.h"
#include "task.h"

#include <unistd.h>

#define TAG "socket"

static int socket_open(void *dev, path_t *path, int flags, mode_t mode) {
    if (path->directory || path->filename)
        return -1;
    return 0;
}

static int socket_close(void *dev, int fd) {
    return close(fd);
}

static ssize_t socket_write(void *dev, int fd, void const *buf, size_t count) {
    return write(fd, buf, count);
}

static ssize_t socket_read(void *dev, int fd, void *buf, size_t count) {
    return read(fd, buf, count);
}

static ssize_t socket_lseek(void *dev, int fd, off_t offset, int whence) {
    return (off_t)-1;
}

device_t *socket_create() {
    socket_device_t *dev = (socket_device_t *)malloc(sizeof(socket_device_t));
    if (!dev) {
        ESP_LOGE(TAG, "Failed to allocate memory for socket device");
        return NULL;
    }

    device_t *base_dev = (device_t *)dev;

    base_dev->type   = DEVICE_TYPE_SOCKET;
    base_dev->_open  = socket_open;
    base_dev->_close = socket_close;
    base_dev->_write = socket_write;
    base_dev->_read  = socket_read;
    base_dev->_lseek = socket_lseek;

    return (device_t *)dev;
}