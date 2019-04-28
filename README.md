<div class="header" align="center">
	<h2>
		<a href="https://rockylim92.github.io/" title="temp">
			<img alt="" src="https://github.com/RockyLim92/copyBox/blob/master/asset/rocky_icon.png" width="100px" height="100px" />
		</a>
		<br />
		oc_bench
	</h2>
	<p align="center">:octocat: oc_bench: A benchmark tool for Open-Channel SSDs using liblightnvm library :octocat:</p>
</div>

---

### Overview
`We designed and implemented FIO-like benchmark tool called oc_bench for Open-Channel SSD. oc bench is a tool for evaluating device performance, changing many parameters, such as number of threads or file size, as well as existing benchmark tools such as fio and iometer. In addition, oc bench can determine physical address of data to be stored by utilizing the features of Open-Channel SSD`


### Contributor(s)
- **Rocky Lim** - [GitHub](https://github.com/RockyLim92)


### Development
- etc.


### Build (or Installation) & Usage

You should edit parameters like NR\_PUNITS, NR\_W\_THREADS, etc. at the code.
It could be organized as configration file or somthing, but it's just a prototype for DapDB.

```
~$ sudo sh make.sh
~$ sudo ./oc_bench
```


### Contents
- `README.md`
- `oc_bench.c`
- `profile.h`

### Acknowledgments
- Matias
- Javier

### References
- [**DapDB: Cutting Latency in Key-Value Store with Dynamic Arrangement of internal Parallelism in SSD**](https://rockylim92.github.io/publication/DapDB.pdf) - (in progress)
- [**RockyLim92 Git**](https://github.com/RockyLim92)
- etc.

