# 集合有限批次验收

独立 Python 标准库客户端，生产服务仍为原生 C++。不提供服务端持续导入任务。

```powershell
python packaging/e2e/collection_batch_client_test.py
python packaging/e2e/collections_e2e.py --binary out/build/windows-msvc-debug/src/server/music-service.exe
python packaging/e2e/collection_batch_client.py --api http://127.0.0.1:18640 --token-file TOKEN_FILE --manifest collection.json --link "https://music.163.com/album?id=512175" --ids 5257138 --wait
```

新集合必须选择 `--all` 或 `--ids ID1,ID2`；保留列表顺序和重复引用。默认每页 50 项、每批 100 项，`--batch-size` 可下调到服务配置上限。默认至多 10000 个选中条目／1000 页；`--max-items`、`--max-pages`、`--stop-after-batches` 显式限制本次工作。

进度清单分别记录 `pages_read`、`fetched_items`、`submitted_items` 和查询后的 `task_entries_by_status`。最后一项按批次位置计数，重复位置可指向同一任务；不表示唯一文件数。`enumeration_finished` 只表示读取到了末页，不表示全部歌曲可下载，也不表示固定时刻快照；`last_page.total`、`listed_total` 与 `unavailable_items` 分开看待。

每批先写入请求体与随机幂等键，原子替换清单，再 POST。应答丢失可使用相同清单 `--replay`；它只重放已保存请求，不重新发现页面。`--monitor` 可另行查询已受理批次；`--wait` 等待所有批次终态，失败与取消均单独统计。清单不含服务 token；token 通过本地文件读取。不要同时让两个客户端写同一清单。

关闭客户端后，已受理任务继续执行；未提交页面不会在后台继续展开。服务重启恢复持久任务，但旧目录游标会失效；本客户端不承诺跨重启继续未提交的分页。需要继续发现时重新开始一轮集合读取，已完成产物按服务的复用规则处理。幂等重放受服务保留期约束，超过期限不保证原批次 ID。

支持集合链接：网易云 `/playlist?id=N`、`/#/album?id=N`、`/playlist/N` 及对应移动路径；酷我 `/playlist_detail/N`、`/album_detail/N`；5sing `/{userId}/dj/{songListId}.html`。网易云短链 `163cn.tv` 暂不支持。

MSVC 构建使用项目预设。配置时会实际探测 `/showIncludes` 前缀并保留原始字节，避免中文语言包被误解码后漏记头文件依赖。已使用错误前缀的旧构建目录需重新配置并执行一次 `cmake --build --preset windows-msvc-debug --clean-first`；之后正常增量构建即可。
