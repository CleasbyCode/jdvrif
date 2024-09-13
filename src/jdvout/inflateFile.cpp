// zlib function, see https://zlib.net/
void inflateFile(std::vector<uint_fast8_t>& Vec) {
	
	uint_fast32_t BUFSIZE = static_cast<uint_fast32_t>(Vec.size());
	uint_fast8_t* buffer{ new uint_fast8_t[BUFSIZE] };

	std::vector<uint_fast8_t>Inflate_Vec;
	
	z_stream strm;
	strm.zalloc = 0;
	strm.zfree = 0;
	strm.next_in = Vec.data();
	strm.avail_in = BUFSIZE;
	strm.next_out = buffer;
	strm.avail_out = BUFSIZE;

	inflateInit(&strm);

	while (strm.avail_in) {
		inflate(&strm, Z_NO_FLUSH);

		if (!strm.avail_out) {
			Inflate_Vec.insert(Inflate_Vec.end(), buffer, buffer + BUFSIZE);
			strm.next_out = buffer;
			strm.avail_out = BUFSIZE;
		} else {
		    break;
		}
	}

	inflate(&strm, Z_FINISH);
	Inflate_Vec.insert(Inflate_Vec.end(), buffer, buffer + BUFSIZE - strm.avail_out);
	inflateEnd(&strm);
	delete[] buffer;
	Vec.swap(Inflate_Vec);
}
