/**
 * sndfile.cpp
 * This file is based on work from the YATE Project http://YATE.null.ro
 *
 * Snd file driver (record+playback)
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2014 Null Team
 *
 * This software is distributed under multiple licenses;
 * see the COPYING file in the main directory for licensing
 * information for this specific distribution.
 *
 * This use of this software may be subject to additional restrictions.
 * See the LEGAL file in the main directory for details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <yatephone.h>

#include <sndfile.h>

#include <string.h>


/*
 * A word about data formats. There are two data formats that we care about
 * when playing or recording from a file. They are the on-disk format that
 * libsndfile reads/writes in and the format of the DataEndpoints use to send
 * data around in Yate.
 *
 * libsndfile's API allows for data to be read/written as PCM shorts, ints,
 * floats or doubles, and it bothers with whatever conversion or encoding is
 * necessary for the on-disk format. Thus, most of the time it makes sense just
 * to set the DataEndpoints to "slin".
 *
 * However, libsndfile also contains a raw read/write functionality, and there
 * are a few situations when an unnecessary conversion can be avoided by
 * exposing the on-disk format into Yate and using raw reads/writes. For
 * example, consider a call with mulaw audio. When playing a file to such a
 * call, if the file is also mulaw, it makes little sense to go
 * mulaw->slin->mulaw, so be reading the data raw and presenting it to yate as
 * mulaw we can avoid the unnecessary conversion. These are special cases
 * though.
 */

using namespace TelEngine;
namespace { // anonymous

static TokenDict sf_subtypes[] = {
    { "slin", SF_FORMAT_PCM_16 },
    { "mulaw", SF_FORMAT_ULAW },
    { "alaw", SF_FORMAT_ALAW },
    { "ima", SF_FORMAT_IMA_ADPCM },
    { "msadpcm", SF_FORMAT_MS_ADPCM },
    { "gsm", SF_FORMAT_GSM610 },
    { "g726", SF_FORMAT_G721_32 },
    { "g721", SF_FORMAT_G721_32 },
    { "g726_24", SF_FORMAT_G723_24 },
    { "g726_32", SF_FORMAT_G721_32 },
    { "g726_40", SF_FORMAT_G723_40 },
    { "g723_24", SF_FORMAT_G723_24 },
    { "g723_32", SF_FORMAT_G721_32 },
    { "g723_40", SF_FORMAT_G723_40 },
    { "vorbis", SF_FORMAT_VORBIS },
    { "A", SF_FORMAT_ALAW },
    { "u", SF_FORMAT_ULAW },
    { "u8", SF_FORMAT_PCM_U8 },
    { "s8", SF_FORMAT_PCM_S8 },
    { "s16", SF_FORMAT_PCM_16 },
    { "s16le", SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE },
    { "s16be", SF_FORMAT_PCM_16 | SF_ENDIAN_BIG },
    { 0, 0 },
};

class SndSource : public ThreadedSource
{
public:
    static SndSource* create(const String& file, CallEndpoint* chan, unsigned maxlen,
	bool autoclose, bool autorepeat, const NamedString* param);
    ~SndSource();
    virtual void run();
    virtual void cleanup();
    virtual bool setFormat(const DataFormat &format);
    virtual void attached(bool added);
    virtual bool control(NamedList& param);
    void setNotify(const String& id);
private:
    SndSource(CallEndpoint* chan, unsigned maxlen, bool autoclose);
    void init(const String& file, bool autorepeat);
    void notify(SndSource* source, const char* reason = 0);

    CallEndpoint* m_chan;
    Stream* m_stream;
    ObjList* m_assets;
    DataBlock m_buffer;
    ObjList* m_commands;

    unsigned m_brate;
    int64_t m_repeatPos;
    unsigned m_total;
    unsigned m_maxlen;
    uint64_t m_time;

    String m_id;

    bool m_autoclose;
    bool m_nodata;

    // libsndfile members
    SNDFILE* m_sndfile;
    SF_INFO m_info;
    bool m_sndfile_raw;
};

class SndConsumer : public DataConsumer
{
public:
    SndConsumer(const String& file, CallEndpoint* chan, unsigned maxlen,
	const char* format, bool append, const NamedString* param);
    ~SndConsumer();
    virtual bool setFormat(const DataFormat& format);
    virtual unsigned long Consume(const DataBlock& data, unsigned long tStamp, unsigned long flags);
    virtual void attached(bool added);
    virtual bool valid() const { return m_valid; }
    //virtual bool control(NamedList& param);
    inline void setNotify(const String& id)
	{ m_id = id; }
private:
    CallEndpoint* m_chan;
    Stream* m_stream;

    bool m_sf_format_set;
    bool m_sf_raw;
    bool m_valid;

    unsigned m_total;
    unsigned m_maxlen;
    uint64_t m_time;
    String m_id;

    String m_filename;
    SNDFILE* m_sndfile;
    SF_INFO m_info;
};

class SndChan : public Channel
{
public:
    SndChan(const String& file, bool record, unsigned maxlen, const NamedList& msg, const NamedString* param = 0);
    ~SndChan();
    bool attachSource(const char* source);
    bool attachConsumer(const char* consumer);
};

class Disconnector : public Thread
{
public:
    Disconnector(CallEndpoint* chan, const String& id, SndSource* source, SndConsumer* consumer, bool disc, const char* reason = 0);
    virtual ~Disconnector();
    virtual void run();
    bool init();
private:
    RefPointer<CallEndpoint> m_chan;
    Message* m_msg;
    SndSource* m_source;
    SndConsumer* m_consumer;
    bool m_disc;
};

class AttachHandler;
class RecordHandler;

class SndFileDriver : public Driver
{
public:
    SndFileDriver();
    virtual void initialize();
    virtual bool msgExecute(Message& msg, String& dest);
    bool unload(bool unloadNow);
protected:
    void statusParams(String& str);
private:
    AttachHandler* m_attachHandler;
    RecordHandler* m_recHandler;
};

Mutex s_mutex(false,"SndFile");
int s_reading = 0;
int s_writing = 0;
bool s_dataPadding = true;
static const Regexp s_formatParser("^([0-9]+\\*)?([^/]+)(/[0-9]+)?$",true,true);

INIT_PLUGIN(SndFileDriver);

UNLOAD_PLUGIN(unloadNow)
{
    return __plugin.unload(unloadNow);
}

static bool parseFormat(const char *str, SF_INFO &info)
{
    String fmt(str);

    if (!fmt.matches(s_formatParser)) {
	Debug(&__plugin,DebugWarn,"Cannot parse \"%s\"", str);
	return false;
    }

    info.samplerate = 8000;
    info.channels = 1;

    fmt.matchString(1) >> info.channels;
    String encoding(fmt.matchString(2));
    fmt.matchString(3) >> "/" >> info.samplerate;

    info.format |= lookup(encoding, sf_subtypes, 0);

    DDebug(&__plugin,DebugAll,"%s %s %s --> %d %x %d \n",
	fmt.matchString(1).c_str(),fmt.matchString(2).c_str(),fmt.matchString(3).c_str(),
	info.channels, info.format, info.samplerate);

    return (info.format & SF_FORMAT_SUBMASK) != 0;
}

class AttachHandler : public MessageHandler
{
public:
    AttachHandler()
	: MessageHandler("chan.attach",100,__plugin.name())
	{ }
    virtual bool received(Message &msg);
};

class RecordHandler : public MessageHandler
{
public:
    RecordHandler()
	: MessageHandler("chan.record",100,__plugin.name())
	{ }
    virtual bool received(Message &msg);
};

static String filename_get_extension(const String& filename)
{
    int off;

    off = filename.rfind('.');
    if (off >= 0)
	return filename.substr(off + 1);
    return String::empty();
}

static int sf_guess_major(const char *extension)
{
    int k, count;
    SF_FORMAT_INFO format_info;

    sf_command(NULL, SFC_GET_FORMAT_MAJOR_COUNT, &count, sizeof(count));
    for (k = 0; k < count; k++) {
	format_info.format = k;
	sf_command(NULL, SFC_GET_FORMAT_MAJOR, &format_info, sizeof(format_info));
	if (!strcasecmp(extension, format_info.extension))
	    return format_info.format;
    }
    /* Annoyingly left out. */
    if (!strcasecmp(extension, "ogg"))
	return SF_FORMAT_OGG;
    return 0;
}

static DataFormat sf_info_to_format(const SF_INFO &m_info)
{
    String s;

    if (m_info.channels > 1)
	s << m_info.channels << '*';

    s << "slin";

    if (m_info.samplerate != 0 && m_info.samplerate != 8000)
	s << '/' << m_info.samplerate;

    DataFormat format(s);

    DDebug(&__plugin,DebugAll, "%dHz, %d channels, --  sf_format 0x%08x, Yate Format: %s",
    m_info.samplerate, m_info.channels, m_info.format, format.c_str());
    return format;
}

static const char* sf_format_id_name(int format)
{
    SF_FORMAT_INFO info;

    info.format = format;
    if (!sf_command(NULL, SFC_GET_FORMAT_INFO, &info, sizeof(info)))
	return info.name;
    return "(invalid)";
}

static sf_count_t yate_vio_get_filelen(void *user_data)
{
    Stream* stream = static_cast<Stream*>(user_data);
    return stream->length();
}

static sf_count_t yate_vio_seek(sf_count_t offset, int whence, void *user_data)
{
    Stream* stream = static_cast<Stream*>(user_data);
    Stream::SeekPos pos;

    switch (whence) {
	case SEEK_CUR: pos = Stream::SeekCurrent; break;
	case SEEK_SET: pos = Stream::SeekBegin; break;
	case SEEK_END: pos = Stream::SeekEnd; break;
	default: return 0;
    }

    return stream->seek(pos, offset);
}

static sf_count_t yate_vio_read(void *ptr, sf_count_t count, void *user_data)
{
    Stream* stream = static_cast<Stream*>(user_data);
    return stream->readData(ptr, count);
}

static sf_count_t yate_vio_write(const void *ptr, sf_count_t count, void *user_data)
{
    Stream* stream = static_cast<Stream*>(user_data);
    return stream->writeData(ptr, count);
}

static sf_count_t yate_vio_tell(void *user_data)
{
    Stream* stream = static_cast<Stream*>(user_data);
    return stream->seek(Stream::SeekCurrent, 0);
}

static SF_VIRTUAL_IO yate_vio = {
    yate_vio_get_filelen,
    yate_vio_seek,
    yate_vio_read,
    yate_vio_write,
    yate_vio_tell
};

SndSource* SndSource::create(const String& file, CallEndpoint* chan, unsigned maxlen, bool autoclose, bool autorepeat, const NamedString* param)
{
    SndSource* tmp = new SndSource(chan,maxlen,autoclose);
    NamedPointer* ptr = YOBJECT(NamedPointer,param);
    if (ptr) {
	Stream* stream = YOBJECT(Stream,ptr);
	if (stream) {
	    DDebug(&__plugin,DebugInfo,"SndSource using Stream %p [%p]",stream,tmp);
	    tmp->m_stream = stream;
	    ptr->takeData();
	}
	else {
	    DataBlock* data = YOBJECT(DataBlock,ptr);
	    if (data) {
		DDebug(&__plugin,DebugInfo,"SndSource using DataBlock %p len=%u [%p]",
		    data,data->length(),tmp);
		tmp->m_stream = new MemoryStream(*data);
	    }
	}
    }
    tmp->init(file,autorepeat);
    return tmp;
}

void SndSource::init(const String& file, bool autorepeat)
{
    if (m_stream) {
	m_sndfile = sf_open_virtual(&yate_vio, SFM_READ, &m_info, m_stream);
    } else {
	if (file == "-") {
	    m_nodata = true;
	    m_info.samplerate = 8000;
	    m_brate = 8000;
	    start("Snd Source");
	    return;
	}

	/* Need a parser */

	m_assets = file.split(',',false);

	String &tmp_file(*static_cast<String*>(m_assets->get()));

	m_info.seekable = 1;
	m_sndfile = sf_open(tmp_file.c_str(), SFM_READ, &m_info);

	/* If the file type couldn't be detected, try and guess based on it's
	** extension.
	**/
	if (!m_sndfile) {
	    const char *s;

	    s = strrchr(tmp_file.c_str(), '/');
	    if (!s)
		s = strrchr(tmp_file.c_str(), '.');
	    else
		s = strrchr(s, '.');
	    if (s)
		s++;
	    if (s) {
		const FormatInfo *fi = NULL;
		String extension(s);

		DDebug(&__plugin,DebugAll,"Looking up extension %s",s);
		fi = FormatRepository::getFormat(extension);

		if (fi && fi->dataRate()) {
		    DDebug(&__plugin,DebugAll,"Found a format info of '%s' (%s)", fi->name, fi->type);
		    m_info.samplerate = fi->sampleRate;
		    m_info.channels = fi->numChannels;
		    m_info.format = SF_FORMAT_RAW | lookup(fi->name, sf_subtypes);
		}
	    }

	    if (!sf_format_check(&m_info)) {
		m_info.samplerate = 8000;
		m_info.channels = 1;
		m_info.format = SF_FORMAT_RAW | SF_FORMAT_PCM_16;
	    }

	    Debug(&__plugin,DebugMild,"Couldn't detect format of '%s', assuming raw %s.",
		tmp_file.c_str(), sf_format_id_name(m_info.format & SF_FORMAT_SUBMASK));

	    m_sndfile = sf_open(tmp_file.c_str(), SFM_READ, &m_info);
	}
    }

    if (!m_sndfile) {
	Debug(&__plugin,DebugWarn,"Opening '%s' for reading: %s",file.c_str(),sf_strerror(NULL));
	notify(this, "error");
	return;
    }

    String native_format;
    const FormatInfo *fi = NULL;
    const char *yate_format_name = lookup(m_info.format & SF_FORMAT_SUBMASK, sf_subtypes);

    /*
     * Initially set our offered format as the raw on-disk form if possible. If
     * the consumers cannot accept it untranslated, ::setFormat() will be called
     * and we will switch back to reading as slin only. This allows use to avoid
     * unnecisarry conversions like mulaw -> slin -> mulaw, but doesn't break
     * things like mulaw/16000 -> slin/16000 -> slin -> mulaw.
     */
    if (yate_format_name) {
        if (m_info.channels != 1)
	    native_format << m_info.channels << "*";
	native_format << yate_format_name;
	if (m_info.samplerate != 8000)
	    native_format << "/" << m_info.samplerate;
	fi = FormatRepository::getFormat(native_format);
    }

    /* Only offer on-disk formats for fixed bit rate formats. */
    if (fi && fi->dataRate() != 0) {
	m_format = native_format;
	m_sndfile_raw = true;
	m_brate = fi->dataRate();
    } else {
	m_format = "";
	if (m_info.channels != 1)
	    m_format << m_info.channels << "*";
	m_format << "slin";
	if (m_info.samplerate != 8000)
	    m_format << "/" << m_info.samplerate;
	m_sndfile_raw = false;
	m_brate = 2 * m_info.channels * m_info.samplerate;
    }

    Debug(&__plugin,DebugInfo,"Playing '%s' as %s", file.c_str(), m_format.c_str());

    XDebug(&__plugin,DebugAll, "\n"
"========== Info ============\n"
"     Frames: %12ld\n"
" SampleRate: %12d Hz\n"
"   Channels: %12d\n"
"     Format:   0x%08x\n"
"   Sections: %12d\n"
"   Seekable: %12d\n"
"Yate Format: %s",
    m_info.frames, m_info.samplerate, m_info.channels, m_info.format, m_info.sections, m_info.seekable, m_format.c_str());

    if (autorepeat) {
	if (m_info.seekable)
	    m_repeatPos = 0;
	else
	    Debug(&__plugin,DebugWarn,"Cannot autorepeat a non-seekable file/stream");
    }
    start("Snd Source");
}

bool SndSource::setFormat(const DataFormat &format)
{
    DDebug(&__plugin,DebugAll,"SndSource::setFormat \"%s\"",format.c_str());

    return false;

    /*
     * If we are here, the on-disk format cannot be used. As the only options
     * are the on-disk formats, or slin of the same channel/samplerate, and
     * translation only really works on slin, switch back to slin.
    if (m_sndfile_raw) {
	m_sndfile_raw = false;
	m_format = "";
	if (m_info.channels != 1)
	    m_format << m_info.channels << "*";
	m_format << "slin";
	if (m_info.samplerate != 8000)
	    m_format << "/" << m_info.samplerate;
	m_brate = 2 * m_info.channels * m_info.samplerate;

	return m_format == format;
    }
     */

    return false;
}

SndSource::SndSource(CallEndpoint* chan, unsigned maxlen, bool autoclose)
    : m_chan(chan), m_stream(0), m_assets(0), m_brate(0), m_repeatPos(-1),
      m_total(0), m_maxlen(maxlen), m_time(0), m_autoclose(autoclose),
      m_nodata(false), m_sndfile(0), m_sndfile_raw(false)
{
    Debug(&__plugin,DebugAll,"SndSource::SndSource(%p) [%p]",chan,this);
    memset(&m_info, 0, sizeof(SF_INFO));
    s_mutex.lock();
    s_reading++;
    s_mutex.unlock();
}

SndSource::~SndSource()
{
    Debug(&__plugin,DebugAll,"SndSource::~SndSource() [%p] total=%u stamp=%lu",this,m_total,timeStamp());
    stop();
    if (m_time) {
        m_time = Time::now() - m_time;
	if (m_time) {
	    m_time = (m_total*(uint64_t)1000000 + m_time/2) / m_time;
	    Debug(&__plugin,DebugInfo,"SndSource rate=" FMT64U " b/s",m_time);
	}
    }
    if (m_sndfile) {
	sf_close(m_sndfile);
	m_sndfile = 0;
    }
    if (m_stream) {
	delete m_stream;
	m_stream = 0;
    }
    if (m_assets) {
	delete m_assets;
	m_assets = 0;
    }
    s_mutex.lock();
    s_reading--;
    s_mutex.unlock();
}

void SndSource::run()
{
    unsigned long ts = 0;
    sf_count_t r = 0;
    // internally reference if used for override or replace purpose
    bool noChan = (0 == m_chan);

    // wait until at least one consumer is attached
    for (;;) {
	lock();
	r = m_consumers.count();
	unlock();
	if (r)
	    break;
	if (!looping(noChan)) {
	    notify(0,"replaced");
	    return;
	}
	Thread::idle();
    }
    DDebug(&__plugin,DebugAll,"Consumer found, starting to play data with rate %d [%p]",m_brate,this);

    unsigned int blen = (m_brate*20)/1000;
    m_buffer.assign(0,blen);
    uint64_t tpos = 0;
    uint64_t last_repeate = 0;
    m_time = tpos;
    unsigned long flags = 0;

    while (looping(noChan)) {

	if (m_maxlen && m_total >= m_maxlen) {
	    /* simulate EOF */
	    goto do_eof;
	}

	if (m_nodata) {
	    flags |= DataNode::DataSilent;
	    r = m_buffer.length();
	} else {
	    if (m_sndfile_raw)
		r = sf_read_raw(m_sndfile, m_buffer.data(), m_buffer.length());
	    else
		r = sf_read_short(m_sndfile, (short *)m_buffer.data(), m_buffer.length()/2)*2;
	}

	// start counting time _after_ the first successful read
	if (!tpos) {
	    flags |= DataNode::DataStart;
	    m_time = tpos = Time::now();
	}

	// Check for EOF. This is the only case that libsndfile returns 0.
	if (!r) {
	    if (m_repeatPos >= 0) {
		if (m_total == last_repeate) {
		    Debug(&__plugin,DebugWarn,"Asked to autorepeat over no data! Exiting.");
		    break;
		}

		if (sf_seek(m_sndfile, m_repeatPos, SEEK_SET) < 0) {
		    Debug(&__plugin,DebugWarn,"Asked to autorepeat, but cannot seek in file! Exiting.");
		    break;
		}

		DDebug(&__plugin,DebugAll,"Autorepeating from offset " FMT64 " [%p]", m_repeatPos, this);

		last_repeate = m_total;
		m_buffer.assign(0,blen);
		flags |= DataNode::DataMark;
		continue;
	    }

	    /* If a prompt, try and load the next file. */
	    if (m_assets) {
		for (m_assets->remove(); m_assets->length(); m_assets->remove()) {
		    String* next_file(static_cast<String *>(m_assets->get()));
		    SF_INFO new_info;

		    if (m_sndfile) {
			sf_close(m_sndfile);
			m_sndfile = NULL;
		    }

		    if (!next_file)
			break;

		    m_sndfile = sf_open(next_file->c_str(), SFM_READ, &new_info);
		    if (m_sndfile) {
			if (new_info.samplerate == m_info.samplerate &&
			    new_info.channels == m_info.channels &&
			    new_info.format == m_info.format) {
			    memcpy(&m_info, &new_info, sizeof(SF_INFO));
			    Debug(&__plugin,DebugAll,"SndSource '%s' playing next file \"%s\"", m_id.c_str(),next_file->c_str());
			    break;
			} else {
			    Debug(&__plugin,DebugWarn,"SndSource '%s' cannot string files of difference formats!", m_id.c_str());
			}
		    } else {
			Debug(&__plugin,DebugWarn,"Opening '%s' for reading: %s",next_file->c_str(),sf_strerror(NULL));
		    }
		}
		if (m_sndfile)
		    continue;
		else
		    break;
	    }
	}

	if (r < (int)m_buffer.length()) {
	    // if desired and possible extend last byte to fill buffer
	    if (s_dataPadding && ((m_format == "mulaw") || (m_format == "alaw"))) {
		unsigned char* d = (unsigned char*)m_buffer.data();
		unsigned char last = d[r-1];
		while (r < (int)m_buffer.length())
		    d[r++] = last;
	    }
	    else
		m_buffer.assign(m_buffer.data(),r);
	}

	int64_t dly = tpos - Time::now();
	if (dly > 0) {
	    XDebug(&__plugin,DebugAll,"SndSource sleeping for " FMT64 " usec",dly);
	    Thread::usleep((unsigned long)dly);
	}

	// Send data to consumers.
	Forward(m_buffer,ts,flags);

	// Account for sent data.
	ts += m_buffer.length()*m_info.samplerate/m_brate;
	m_total += r;
	tpos += (r*(uint64_t)1000000/m_brate);
	flags = 0;
    }

    m_buffer.clear();
    if (r) {
	notify(0, "replaced");
    } else {
do_eof:
	Debug(&__plugin,DebugAll,"SndSource '%s' end of data (%u played) chan=%p [%p]",
	    m_id.c_str(),m_total,m_chan,this);
	Forward(m_buffer, ts, (unsigned long)DataNode::DataEnd);
	notify(this, "eof");
    }
}

void SndSource::cleanup()
{
    RefPointer<CallEndpoint> chan;
    if (m_chan) {
	DataEndpoint::commonMutex().lock();
	chan = m_chan;
	m_chan = 0;
	DataEndpoint::commonMutex().unlock();
    }
    Debug(&__plugin,DebugAll,"SndSource cleanup, total=%u, chan=%p [%p]",
	m_total,(void*)chan,this);
    if (chan)
	chan->clearData(this);
    ThreadedSource::cleanup();
}

void SndSource::attached(bool added)
{
    if (!added && m_chan && !m_chan->alive()) {
	DDebug(&__plugin,DebugInfo,"SndSource clearing dead chan %p [%p]",m_chan,this);
	m_chan = 0;
    }
}

bool SndSource::control(NamedList &param)
{
    DDebug(&__plugin,DebugAll,"SndSource::control [%p]",this);
    /* TODO: Flesh this out. */
    return false;
}

void SndSource::setNotify(const String& id)
{
    m_id = id;
    if (!(m_stream || m_sndfile || m_nodata))
	notify(this, "null");
}

void SndSource::notify(SndSource* source, const char* reason)
{
    RefPointer<CallEndpoint> chan;
    if (m_chan) {
	DataEndpoint::commonMutex().lock();
	if (source)
	    chan = m_chan;
	m_chan = 0;
	DataEndpoint::commonMutex().unlock();
    }
    if (!chan) {
	if (m_id) {
	    DDebug(&__plugin,DebugAll,"SndSource enqueueing notify message [%p]",this);
	    Message* m = new Message("chan.notify");
	    m->addParam("targetid",m_id);
	    if (reason)
		m->addParam("reason",reason);
	    Engine::enqueue(m);
	}
    }
    else if (m_id || m_autoclose) {
	DDebug(&__plugin,DebugInfo,"Preparing '%s' disconnector for '%s' chan %p '%s' source=%p [%p]",
	    reason,m_id.c_str(),(void*)chan,chan->id().c_str(),source,this);
	Disconnector *disc = new Disconnector(chan,m_id,source,0,m_autoclose,reason);
	if (!disc->init() && m_autoclose)
	    chan->clearData(source);
    }
    stop();
}

SndConsumer::SndConsumer(const String& file, CallEndpoint* chan, unsigned maxlen,
    const char* format, bool append, const NamedString* param)
    : m_chan(chan),
      m_stream(0),
      m_sf_format_set(false),
      m_sf_raw(false),
      m_valid(true),
      m_total(0),
      m_maxlen(maxlen),
      m_time(0),
      m_filename(file),
      m_sndfile(0)
{
    Debug(&__plugin,DebugAll,"SndConsumer::SndConsumer(\"%s\",%p,%u,\"%s\",%s,%p) [%p]",
	file.c_str(),chan,maxlen,format,String::boolText(append),param,this);

    s_mutex.lock();
    s_writing++;
    s_mutex.unlock();

    memset(&m_info, 0, sizeof(m_info));

/*  TODO
    NamedPointer* ptr = YOBJECT(NamedPointer,param);
    if (ptr) {
	m_stream = YOBJECT(Stream,ptr);
	if (m_stream) {
	    DDebug(&__plugin,DebugInfo,"SndConsumer using Stream %p [%p]",m_stream,this);
	    ptr->takeData();
	}
    }
*/
    /* Try and get the filetype & encoding from the file to append. */
    if (append && File::exists(file.c_str()) && !format) {
	m_sndfile = sf_open(file.c_str(), SFM_RDWR, &m_info);

	if (m_sndfile) {
	    if (sf_seek(m_sndfile, 0, SEEK_END != 0)) {
		Debug(DebugWarn,"Cannot seek in %s: %s\n", file.c_str(),sf_strerror(m_sndfile));
		m_valid = false;
		return;
	    }
	    m_sf_format_set = true;
	}
    }

    if (!m_sndfile) {
	String extension(filename_get_extension(file).toLower());

	/* Guess the filetype (container) from the filename. */
	if (!(m_info.format & SF_FORMAT_TYPEMASK)) {
	    int type = sf_guess_major(extension.c_str());
	    if (type)
		m_info.format |= type;
	    else
		m_info.format |= SF_FORMAT_RAW;
	}

	/* User-specified on-disk encoding. */
	if (format && parseFormat(format, m_info)) {
	    if (m_info.format & SF_FORMAT_SUBMASK) {
		if (sf_format_check(&m_info)) {
		    if (m_info.channels && m_info.samplerate)
			m_sf_format_set = true;
		} else {
		    Debug(&__plugin,DebugMild,"Combination of %s in %s is not valid!",
			sf_format_id_name(m_info.format & SF_FORMAT_SUBMASK),
			sf_format_id_name(m_info.format & SF_FORMAT_TYPEMASK));
		    m_info.format &= ~SF_FORMAT_SUBMASK;
		}
	    }
	}

	/* If raw and we still don't have an encoding, try and use the extension
	 * as a hint. */
	if (m_info.format == SF_FORMAT_RAW)
	    m_info.format |= lookup(extension, sf_subtypes, 0);

	if (append && File::exists(file.c_str())) {
	    m_sndfile = sf_open(file.c_str(), SFM_RDWR, &m_info);
	    if (!m_sndfile || sf_seek(m_sndfile, 0, SEEK_END != 0)) {
		Debug(DebugWarn,"Cannot append to %s: %s",file.c_str(),sf_strerror(m_sndfile));
		m_valid = false;
		return;
	    } else {
		m_sf_format_set = true;
	    }
	}
    }

    if (m_sf_format_set) {
	m_format = sf_info_to_format(m_info);
    }
}

SndConsumer::~SndConsumer()
{
    Debug(&__plugin,DebugAll,"SndConsumer::~SndConsumer() [%p] total=%u stamp=%lu",this,m_total,timeStamp());
    if (m_time) {
        m_time = Time::now() - m_time;
	if (m_time) {
	    m_time = (m_total*(uint64_t)1000000 + m_time/2) / m_time;
	    Debug(&__plugin,DebugInfo,"SndConsumer rate=" FMT64U " b/s",m_time);
	}
    }
    if (m_sndfile)
	sf_close(m_sndfile);
    if (m_stream)
	delete m_stream;
    m_stream = 0;


    s_mutex.lock();
    s_writing--;
    s_mutex.unlock();
}

/*
 * This is the format the other endpoint wants to send us, not necessarily what
 * encoding the file is. If sf_format_set is still false, try and use this
 * format for the on-disk encoding.
 *
 * Also, the return value is special. True if the format changed, false
 * if it didn't, either because we don't support it or it was the same.
 */
bool SndConsumer::setFormat(const DataFormat& format)
{
    SF_INFO ep_info = { 0 };

    DDebug(&__plugin,DebugAll,"SndConsumer::setFormat \"%s\"",format.c_str());

    parseFormat(format.c_str(), ep_info);

    /*
     * If we haven't yet set an on-disk encoding/rate/channel, try to match
     * this as closely as possible.
     */
    if (!m_sf_format_set) {
	int subfmt = m_info.format & SF_FORMAT_SUBMASK;

	if (!m_info.channels && ep_info.channels)
	    m_info.channels = ep_info.channels;

	if (!m_info.samplerate && ep_info.samplerate)
	    m_info.samplerate = ep_info.samplerate;

	if (!subfmt && ep_info.format) {
	    m_info.format |= ep_info.format;
	    if (!sf_format_check(&m_info)) {
		m_info.format &= ~SF_FORMAT_SUBMASK;
	    }
	}
    }

    /*
     * Check to see if the format the endpoint is offering is *exactly*
     * identical to the on-disk format. If so, we can do raw writes and avoid
     * an a->b->a series of conversions.
     *
     * Exception: Endianness of PCM. As writing it through shorts is basically
     * raw when endianness is the same, just ignore it.
     */
    m_sf_raw = (ep_info.format != SF_FORMAT_PCM_16 &&
		ep_info.format && ep_info.channels && ep_info.samplerate &&
		ep_info.format == (m_info.format & SF_FORMAT_SUBMASK) &&
		ep_info.channels == m_info.channels &&
		ep_info.samplerate == m_info.samplerate);

    if (m_format != format) {
	DataFormat tmp(m_format);
	if (m_sf_raw)
	    m_format = format;
	else
	    m_format = sf_info_to_format(m_info);
	Debug(&__plugin,DebugInfo,"Consumer format changed from %s to %s",tmp.c_str(), m_format.c_str());
	return m_format == format;
    }

    /* Return false if nothing changed (reguardless of success or failure.) */
    return false;
}

unsigned long SndConsumer::Consume(const DataBlock& data, unsigned long tStamp, unsigned long flags)
{
    if (!m_sndfile) {
	if (!m_sf_format_set) {
	    if (!m_info.channels)
		m_info.channels = m_format.numChannels(1);
	    if (!m_info.samplerate)
		m_info.samplerate = m_format.sampleRate(8000);

	    /* Hunt for a subtype that fits in this encoding. */
	    if (!(m_info.format & SF_FORMAT_SUBMASK)) {
		SF_FORMAT_INFO info;
		int k, count;

		sf_command(NULL, SFC_GET_FORMAT_SUBTYPE_COUNT, &count, sizeof(count));
		for (k = 0; k < count; k++) {
		    info.format = k;
		    sf_command(NULL, SFC_GET_FORMAT_SUBTYPE, &info, sizeof(info));

		    /* Hack: skip 8-bit audio... */
		    if (info.format == SF_FORMAT_PCM_S8)
			continue;

		    m_info.format = (m_info.format & SF_FORMAT_TYPEMASK) | info.format;
		    if (sf_format_check(&m_info))
			break;
		}
	    }
	    m_sf_format_set = true;
	}

	Debug(&__plugin,DebugInfo,"Opened %s file \"%s\" for writing %s (%dHz %s)",
	    sf_format_id_name(m_info.format & SF_FORMAT_TYPEMASK),
	    m_filename.c_str(),
	    sf_format_id_name(m_info.format & SF_FORMAT_SUBMASK),
	    m_info.samplerate, m_info.channels == 1 ? "mono" : m_info.channels == 2 ? "stereo" : "multi-channel");
	m_sndfile = sf_open(m_filename.c_str(), SFM_WRITE, &m_info);
	if (!m_sndfile) {
	    Debug(&__plugin,DebugWarn,"Could not open \"%s\" for writing: %s",m_filename.c_str(),sf_strerror(m_sndfile));
	    m_valid = 0;
	    return 0;
	}
    }
    if (!data.null()) {
	sf_count_t nsamp;
	if (!m_time)
	    m_time = Time::now();
	if (m_sf_raw)
	    nsamp = sf_write_raw(m_sndfile, data.data(), data.length());
	else
	    nsamp = sf_write_short(m_sndfile, (short *)data.data(), data.length() / 2) * 2;
	m_total += nsamp;
	if (m_maxlen && (m_total >= m_maxlen)) {
	    m_maxlen = 0;
	    delete m_stream;
	    m_stream = 0;
	    RefPointer<CallEndpoint> chan;
	    if (m_chan) {
		DataEndpoint::commonMutex().lock();
		chan = m_chan;
		m_chan = 0;
		DataEndpoint::commonMutex().unlock();
	    }
	    if (chan) {
		DDebug(&__plugin,DebugInfo,"Preparing 'maxlen' disconnector for '%s' chan %p '%s' in consumer [%p]",
		    m_id.c_str(),(void*)chan,chan->id().c_str(),this);
		Disconnector *disc = new Disconnector(chan,m_id,0,this,false,"maxlen");
		disc->init();
	    }
	}
	return nsamp == data.length() ? invalidStamp() : nsamp;
    }
    return 0;
}

void SndConsumer::attached(bool added)
{
    if (!added && m_chan && !m_chan->alive()) {
	DDebug(&__plugin,DebugInfo,"SndConsumer clearing dead chan %p [%p]",m_chan,this);
	m_chan = 0;
    }
}

Disconnector::Disconnector(CallEndpoint* chan, const String& id, SndSource* source, SndConsumer* consumer, bool disc, const char* reason)
    : Thread("SndDisconnector"),
      m_chan(chan), m_msg(0), m_source(0), m_consumer(consumer), m_disc(disc)
{
    if (id) {
	Message* m = new Message("chan.notify");
	if (m_chan)
	    m->addParam("id",m_chan->id());
	m->addParam("targetid",id);
	if (reason)
	    m->addParam("reason",reason);
	m->userData(m_chan);
	m_msg = m;
    }
}

Disconnector::~Disconnector()
{
    if (m_msg) {
	DDebug(&__plugin,DebugAll,"Disconnector enqueueing notify message [%p]",this);
	Engine::enqueue(m_msg);
    }
}

bool Disconnector::init()
{
    if (error() || !startup()) {
	Debug(&__plugin,DebugGoOn,"Error starting disconnector thread %p",this);
	delete this;
	return false;
    }
    return true;
}

void Disconnector::run()
{
    DDebug(&__plugin,DebugAll,"Disconnector::run() chan=%p msg=%p source=%p disc=%s [%p]",
	(void*)m_chan,m_msg,m_source,String::boolText(m_disc),this);
    if (!m_chan)
	return;
    if (m_source) {
	if (!m_chan->clearData(m_source))
	    Debug(&__plugin,DebugNote,"Source %p in channel %p was replaced with %p",
		m_source,(void*)m_chan,m_chan->getSource());
	if (m_disc)
	    m_chan->disconnect("eof");
    }
    else {
	if (m_msg)
	    m_chan->clearData(m_consumer);
	else
	    m_chan->disconnect();
    }
}


SndChan::SndChan(const String& file, bool record, unsigned maxlen, const NamedList& msg, const NamedString* param)
    : Channel(__plugin)
{
    Debug(this,DebugAll,"SndChan::SndChan(%s) [%p]",(record ? "record" : "play"),this);
    if (record) {
	setConsumer(new SndConsumer(file,this,maxlen,msg.getValue("format"),
	    msg.getBoolValue("append"),param));
	getConsumer()->deref();
    }
    else {
	setSource(SndSource::create(file,this,maxlen,true,msg.getBoolValue("autorepeat"),param));
	getSource()->deref();
    }
}

SndChan::~SndChan()
{
    Debug(this,DebugAll,"SndChan::~SndChan() %s [%p]",id().c_str(),this);
}

bool SndChan::attachSource(const char* source)
{
    if (TelEngine::null(source))
	return false;
    Message m("chan.attach");
    m.userData(this);
    m.addParam("id",id());
    m.addParam("source",source);
    m.addParam("single",String::boolText(true));
    return Engine::dispatch(m);
}

bool SndChan::attachConsumer(const char* consumer)
{
    if (TelEngine::null(consumer))
	return false;
    Message m("chan.attach");
    m.userData(this);
    m.addParam("id",id());
    m.addParam("consumer",consumer);
    m.addParam("single",String::boolText(true));
    return Engine::dispatch(m);
}

static const Regexp s_destExp("^snd/([^/]*+)/(.*)$", true);

bool AttachHandler::received(Message &msg)
{
    int more = 4;
    String src(msg.getValue("source"));
    if (src.null())
	more--;
    else {
	if (src.matches(s_destExp)) {
	    if (src.matchString(1) == "play") {
		src = src.matchString(2);
		more--;
	    }
	    else {
		Debug(DebugWarn,"Could not attach source with method '%s', use 'play'",
		    src.matchString(1).c_str());
		src.clear();
	    }
	}
	else
	    src.clear();
    }

    String cons(msg.getValue("consumer"));
    if (cons.null())
	more--;
    else {
	if (cons.matches(s_destExp)) {
	    if (cons.matchString(1) == "record") {
		cons = cons.matchString(2);
		more--;
	    }
	    else {
		Debug(DebugWarn,"Could not attach consumer with method '%s', use 'record'",
		    cons.matchString(1).c_str());
		cons.clear();
	    }
	}
	else
	    cons.clear();
    }

    String ovr(msg.getValue("override"));
    if (ovr.null())
	more--;
    else {
	if (ovr.matches(s_destExp)) {
	    if (ovr.matchString(1) == "play") {
		ovr = ovr.matchString(2);
		more--;
	    }
	    else {
		Debug(DebugWarn,"Could not attach override with method '%s', use 'play'",
		    ovr.matchString(1).c_str());
		ovr.clear();
	    }
	}
	else
	    ovr.clear();
    }

    String repl(msg.getValue("replace"));
    if (repl.null())
	more--;
    else {
	if (repl.matches(s_destExp)) {
	    if (repl.matchString(1) == "play") {
		repl = repl.matchString(2);
		more--;
	    }
	    else {
		Debug(DebugWarn,"Could not attach replacement with method '%s', use 'play'",
		    repl.matchString(1).c_str());
		repl.clear();
	    }
	}
	else
	    repl.clear();
    }

    if (src.null() && cons.null() && ovr.null() && repl.null())
	return false;

    // if single attach was requested we can return true if everything is ok
    bool ret = msg.getBoolValue("single");

    CallEndpoint* ch = YOBJECT(CallEndpoint,msg.userData());
    if (!ch) {
	if (!src.null())
	    Debug(DebugWarn,"Snd source '%s' attach request with no data channel!",src.c_str());
	if (!cons.null())
	    Debug(DebugWarn,"Snd consumer '%s' attach request with no data channel!",cons.c_str());
	if (!ovr.null())
	    Debug(DebugWarn,"Snd override '%s' attach request with no data channel!",ovr.c_str());
	return false;
    }

    const char* notify = msg.getValue("notify");
    unsigned int maxlen = msg.getIntValue("maxlen");

    if (!src.null()) {
	if (src == "-") {
	    /* clear source */
	    ch->setSource();
	} else {
	    SndSource* s = SndSource::create(src,ch,maxlen,false,msg.getBoolValue("autorepeat"),msg.getParam("source"));
	    ch->setSource(s);
	    s->setNotify(msg.getValue("notify_source",notify));
	    s->deref();
	    msg.clearParam("source");
	}
    }

    if (!cons.null()) {
	if (cons == "-") {
	    ch->setConsumer();
	} else {
	    SndConsumer* c = new SndConsumer(cons,ch,maxlen,msg.getValue("format"),
		msg.getBoolValue("append"),msg.getParam("consumer"));
	    c->setNotify(msg.getValue("notify_consumer",notify));
	    ch->setConsumer(c);
	    c->deref();
	    msg.clearParam("consumer");
	}
    }

    while (!ovr.null()) {
	DataEndpoint::commonMutex().lock();
	RefPointer<DataConsumer> c = ch->getConsumer();
	DataEndpoint::commonMutex().unlock();
	if (!c) {
	    Debug(DebugWarn,"Snd override '%s' attach request with no consumer!",ovr.c_str());
	    ret = false;
	    break;
	}
	SndSource* s = SndSource::create(ovr,0,maxlen,false,msg.getBoolValue("autorepeat"),msg.getParam("override"));
	s->setNotify(msg.getValue("notify_override",notify));
	if (DataTranslator::attachChain(s,c,true))
	    msg.clearParam("override");
	else {
	    Debug(DebugWarn,"Failed to override attach wave '%s' to consumer %p",
		ovr.c_str(),(void*)c);
	    ret = false;
	}
	s->deref();
	break;
    }

    while (!repl.null()) {
	DataEndpoint::commonMutex().lock();
	RefPointer<DataConsumer> c = ch->getConsumer();
	DataEndpoint::commonMutex().unlock();
	if (!c) {
	    Debug(DebugWarn,"Snd replacement '%s' attach request with no consumer!",repl.c_str());
	    ret = false;
	    break;
	}
	SndSource* s = SndSource::create(repl,0,maxlen,false,msg.getBoolValue("autorepeat"),msg.getParam("replace"));
	s->setNotify(msg.getValue("notify_replace",notify));
	if (DataTranslator::attachChain(s,c,false))
	    msg.clearParam("replace");
	else {
	    Debug(DebugWarn,"Failed to replacement attach wave '%s' to consumer %p",
		repl.c_str(),(void*)c);
	    ret = false;
	}
	s->deref();
	break;
    }

    // Stop dispatching if we handled all requested
    return ret && !more;
}


bool RecordHandler::received(Message &msg)
{
    int more = 2;

    String c1(msg.getValue("call"));
    if (c1.null())
	more--;
    else {
	if (c1.matches(s_destExp)) {
	    if (c1.matchString(1) == "record") {
		c1 = c1.matchString(2);
		more--;
	    }
	    else {
		Debug(DebugWarn,"Could not attach call recorder with method '%s', use 'record'",
		    c1.matchString(1).c_str());
		c1.clear();
	    }
	}
	else
	    c1.clear();
    }

    String c2(msg.getValue("peer"));
    if (c2.null())
	more--;
    else {
	if (c2.matches(s_destExp)) {
	    if (c2.matchString(1) == "record") {
		c2 = c2.matchString(2);
		more--;
	    }
	    else {
		Debug(DebugWarn,"Could not attach peer recorder with method '%s', use 'record'",
		    c2.matchString(1).c_str());
		c2.clear();
	    }
	}
	else
	    c2.clear();
    }

    if (c1.null() && c2.null())
	return false;

    const char* notify = msg.getValue("notify");
    unsigned int maxlen = msg.getIntValue("maxlen");

    CallEndpoint* ch = YOBJECT(CallEndpoint,msg.userData());
    RefPointer<DataEndpoint> de = YOBJECT(DataEndpoint,msg.userData());
    if (ch && !de)
	de = ch->setEndpoint();

    if (!de) {
	if (!c1.null())
	    Debug(DebugWarn,"Snd source '%s' call record with no data channel!",c1.c_str());
	if (!c2.null())
	    Debug(DebugWarn,"Snd source '%s' peer record with no data channel!",c2.c_str());
	return false;
    }

    bool append = msg.getBoolValue("append");
    const char* format = msg.getValue("format");
    if (!c1.null()) {
	SndConsumer* c = new SndConsumer(c1,ch,maxlen,format,append,msg.getParam("call"));
	c->setNotify(msg.getValue("notify_call",notify));
	de->setCallRecord(c);
	c->deref();
    }

    if (!c2.null()) {
	SndConsumer* c = new SndConsumer(c2,ch,maxlen,format,append,msg.getParam("peer"));
	c->setNotify(msg.getValue("notify_peer",notify));
	de->setPeerRecord(c);
	c->deref();
    }

    // Stop dispatching if we handled all requested
    return !more;
}


bool SndFileDriver::msgExecute(Message& msg, String& dest)
{
    static const Regexp r("^(play|record)/(.*)$", true);
    if (!dest.matches(r)) {
	Debug(DebugWarn,"Invalid wavefile destination '%s', use 'record/' or 'play/'",
	    dest.c_str());
	return false;
    }

    bool rec_meth = (dest.matchLength(1) == 6); // Record

    unsigned int maxlen = msg.getIntValue("maxlen");
    CallEndpoint* ch = YOBJECT(CallEndpoint,msg.userData());
    if (ch) {
	Debug(this,DebugInfo,"%s sndfile '%s'", (rec_meth ? "Record to" : "Play from"),
	    dest.matchString(2).c_str());
	SndChan *c = new SndChan(dest.matchString(2),rec_meth,maxlen,msg,msg.getParam("callto"));
	c->initChan();
	if (rec_meth)
	    c->attachSource(msg.getValue("source"));
	else
	    c->attachConsumer(msg.getValue("consumer"));
	if (ch->connect(c,msg.getValue("reason"))) {
	    c->callConnect(msg);
	    msg.setParam("peerid",c->id());
	    c->deref();
	    return true;
	}
	else {
	    c->destruct();
	    return false;
	}
    }
    Message m("call.route");
    m.copyParams(msg,msg[YSTRING("copyparams")]);
    m.clearParam(YSTRING("callto"));
    m.clearParam(YSTRING("id"));
    m.setParam("module",name());
    m.setParam("cdrtrack",String::boolText(false));
    m.copyParam(msg,YSTRING("called"));
    m.copyParam(msg,YSTRING("caller"));
    m.copyParam(msg,YSTRING("callername"));
    String callto(msg.getValue(YSTRING("direct")));
    if (callto.null()) {
	const char *targ = msg.getValue(YSTRING("target"));
	if (!targ)
	    targ = msg.getValue(YSTRING("called"));
	if (!targ) {
	    Debug(DebugWarn,"Snd outgoing call with no target!");
	    return false;
	}
	m.setParam("called",targ);
	if (!m.getValue(YSTRING("caller")))
	    m.setParam("caller",prefix() + dest);
	if (!Engine::dispatch(m)) {
	    Debug(DebugWarn,"Snd outgoing call but no route!");
	    return false;
	}
	callto = m.retValue();
	m.retValue().clear();
    }
    m = "call.execute";
    m.setParam("callto",callto);
    SndChan *c = new SndChan(dest.matchString(2),rec_meth,maxlen,msg);
    c->initChan();
    if (rec_meth)
	c->attachSource(msg.getValue("source"));
    else
	c->attachConsumer(msg.getValue("consumer"));
    m.setParam("id",c->id());
    m.userData(c);
    if (Engine::dispatch(m)) {
	msg.setParam("id",c->id());
	msg.copyParam(m,YSTRING("peerid"));
	c->deref();
	return true;
    }
    Debug(DebugWarn,"Snd outgoing call not accepted!");
    c->destruct();
    return false;
}

void SndFileDriver::statusParams(String& str)
{
    str.append("play=",",") << s_reading;
    str << ",record=" << s_writing;
    Driver::statusParams(str);
}

SndFileDriver::SndFileDriver()
    : Driver("snd","misc"), m_attachHandler(0), m_recHandler(0)
{
    char buffer[128];

    sf_command(NULL, SFC_GET_LIB_VERSION, buffer, sizeof(buffer));

    Output("Loaded module SndFile - Based on %s", buffer);
}

void SndFileDriver::initialize()
{
    Output("Initializing module SndFile");
    setup();
    s_dataPadding = Engine::config().getBoolValue("hacks","datapadding",true);
    if (!m_attachHandler) {
	Engine::install((m_attachHandler = new AttachHandler));
	Engine::install((m_recHandler = new RecordHandler));
    }
}

bool SndFileDriver::unload(bool unloadNow)
{
    s_mutex.lock();
    if (s_reading || s_writing) {
	s_mutex.unlock();
	return false;
    }
    s_mutex.unlock();

    Engine::uninstall(m_attachHandler);
    Engine::uninstall(m_recHandler);
    uninstallRelays();

    return true;
}

}; // anonymous namespace

/* vi: set ts=8 sw=4 sts=4 noet: */
