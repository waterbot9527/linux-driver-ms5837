# ms5837 Linux 驱动

此驱动已弃用，树莓派内核代码中有适配的驱动,兼容 ms5837:
```bash
/develop/aaron/raspberry/raspberry/linux/drivers/iio/pressure/ms5637.c
```
我在树莓派系统中的目录 `/etc/modules` 增加了自动加载驱动。

**设备树的配置是需要的，否则不会加载此驱动。**
## 设备树配置
如果你的系统使用设备树，请将 `ms5837-overlay.dts` 编译为 `.dtbo` 并加载：

```sh
dtc -@ -I dts -O dtb -o ms5837-overlay.dtbo ms5837-overlay.dts
sudo cp ms5837-overlay.dtbo /boot/firmware/overlays/
echo "dtoverlay=ms5837-overlay" | sudo tee -a /boot/firmware/config.txt
```
