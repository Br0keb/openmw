#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <vector>

#include <boost/thread.hpp>

#include "openal_output.hpp"
#include "sound_decoder.hpp"
#include "sound.hpp"
#include "soundmanager.hpp"


namespace MWSound
{

static void fail(const std::string &msg)
{ throw std::runtime_error("OpenAL exception: " + msg); }

static void throwALCerror(ALCdevice *device)
{
    ALCenum err = alcGetError(device);
    if(err != ALC_NO_ERROR)
        fail(alcGetString(device, err));
}

static void throwALerror()
{
    ALenum err = alGetError();
    if(err != AL_NO_ERROR)
        fail(alGetString(err));
}


static ALenum getALFormat(ChannelConfig chans, SampleType type)
{
    static const struct {
        ALenum format;
        ChannelConfig chans;
        SampleType type;
    } fmtlist[] = {
        { AL_FORMAT_MONO16,   ChannelConfig_Mono,   SampleType_Int16 },
        { AL_FORMAT_MONO8,    ChannelConfig_Mono,   SampleType_UInt8 },
        { AL_FORMAT_STEREO16, ChannelConfig_Stereo, SampleType_Int16 },
        { AL_FORMAT_STEREO8,  ChannelConfig_Stereo, SampleType_UInt8 },
    };
    static const size_t fmtlistsize = sizeof(fmtlist)/sizeof(fmtlist[0]);

    for(size_t i = 0;i < fmtlistsize;i++)
    {
        if(fmtlist[i].chans == chans && fmtlist[i].type == type)
            return fmtlist[i].format;
    }
    fail(std::string("Unsupported sound format (")+getChannelConfigName(chans)+", "+getSampleTypeName(type)+")");
    return AL_NONE;
}

//
// A streaming OpenAL sound.
//
class OpenAL_SoundStream : public Sound
{
    static const ALuint sNumBuffers = 6;
    static const ALfloat sBufferLength = 0.125f;

    OpenAL_Output &mOutput;

    ALuint mSource;
    ALuint mBuffers[sNumBuffers];

    ALenum mFormat;
    ALsizei mSampleRate;
    ALuint mBufferSize;

    DecoderPtr mDecoder;

    volatile bool mIsFinished;

public:
    OpenAL_SoundStream(OpenAL_Output &output, ALuint src, DecoderPtr decoder);
    virtual ~OpenAL_SoundStream();

    virtual void stop();
    virtual bool isPlaying();
    virtual void update(const float *pos);

    void play();
    bool process();
};

//
// A background streaming thread (keeps active streams processed)
//
struct OpenAL_Output::StreamThread {
    typedef std::vector<OpenAL_SoundStream*> StreamVec;
    StreamVec mStreams;
    boost::mutex mMutex;
    boost::thread mThread;

    StreamThread()
      : mThread(boost::ref(*this))
    {
    }
    ~StreamThread()
    {
        mThread.interrupt();
    }

    // boost::thread entry point
    void operator()()
    {
        while(1)
        {
            mMutex.lock();
            StreamVec::iterator iter = mStreams.begin();
            while(iter != mStreams.end())
            {
                if((*iter)->process() == false)
                    iter = mStreams.erase(iter);
                else
                    iter++;
            }
            mMutex.unlock();
            boost::this_thread::sleep(boost::posix_time::milliseconds(50));
        }
    }

    void add(OpenAL_SoundStream *stream)
    {
        mMutex.lock();
        if(std::find(mStreams.begin(), mStreams.end(), stream) == mStreams.end())
            mStreams.push_back(stream);
        mMutex.unlock();
    }

    void remove(OpenAL_SoundStream *stream)
    {
        mMutex.lock();
        StreamVec::iterator iter = std::find(mStreams.begin(), mStreams.end(), stream);
        if(iter != mStreams.end())
            mStreams.erase(iter);
        mMutex.unlock();
    }

    void removeAll()
    {
        mMutex.lock();
        mStreams.clear();
        mMutex.unlock();
    }
};


OpenAL_SoundStream::OpenAL_SoundStream(OpenAL_Output &output, ALuint src, DecoderPtr decoder)
  : mOutput(output), mSource(src), mDecoder(decoder), mIsFinished(true)
{
    throwALerror();

    alGenBuffers(sNumBuffers, mBuffers);
    throwALerror();
    try
    {
        int srate;
        ChannelConfig chans;
        SampleType type;

        mDecoder->getInfo(&srate, &chans, &type);
        mFormat = getALFormat(chans, type);
        mSampleRate = srate;

        mBufferSize = static_cast<ALuint>(sBufferLength*srate);
        mBufferSize = framesToBytes(mBufferSize, chans, type);
    }
    catch(std::exception &e)
    {
        mOutput.mFreeSources.push_back(mSource);
        alDeleteBuffers(sNumBuffers, mBuffers);
        alGetError();
        throw;
    }
}
OpenAL_SoundStream::~OpenAL_SoundStream()
{
    mOutput.mStreamThread->remove(this);

    alSourceStop(mSource);
    alSourcei(mSource, AL_BUFFER, 0);

    mOutput.mFreeSources.push_back(mSource);
    alDeleteBuffers(sNumBuffers, mBuffers);
    alGetError();

    mDecoder->close();
}

void OpenAL_SoundStream::play()
{
    std::vector<char> data(mBufferSize);

    alSourceStop(mSource);
    alSourcei(mSource, AL_BUFFER, 0);
    throwALerror();

    for(ALuint i = 0;i < sNumBuffers;i++)
    {
        size_t got;
        got = mDecoder->read(&data[0], data.size());
        alBufferData(mBuffers[i], mFormat, &data[0], got, mSampleRate);
    }
    throwALerror();

    alSourceQueueBuffers(mSource, sNumBuffers, mBuffers);
    alSourcePlay(mSource);
    throwALerror();

    mIsFinished = false;
    mOutput.mStreamThread->add(this);
}

void OpenAL_SoundStream::stop()
{
    mOutput.mStreamThread->remove(this);
    mIsFinished = true;

    alSourceStop(mSource);
    alSourcei(mSource, AL_BUFFER, 0);
    throwALerror();

    mDecoder->rewind();
}

bool OpenAL_SoundStream::isPlaying()
{
    ALint state;

    alGetSourcei(mSource, AL_SOURCE_STATE, &state);
    throwALerror();

    if(state == AL_PLAYING)
        return true;
    return !mIsFinished;
}

void OpenAL_SoundStream::update(const float *pos)
{
    alSource3f(mSource, AL_POSITION, pos[0], pos[2], -pos[1]);
    alSource3f(mSource, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
    alSource3f(mSource, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    throwALerror();
}

bool OpenAL_SoundStream::process()
{
    ALint processed, state;

    alGetSourcei(mSource, AL_SOURCE_STATE, &state);
    alGetSourcei(mSource, AL_BUFFERS_PROCESSED, &processed);
    throwALerror();

    if(processed > 0)
    {
        std::vector<char> data(mBufferSize);
        do {
            ALuint bufid;
            size_t got;

            alSourceUnqueueBuffers(mSource, 1, &bufid);
            processed--;

            if(mIsFinished)
                continue;

            got = mDecoder->read(&data[0], data.size());
            mIsFinished = (got < data.size());
            if(got > 0)
            {
                alBufferData(bufid, mFormat, &data[0], got, mSampleRate);
                alSourceQueueBuffers(mSource, 1, &bufid);
            }
        } while(processed > 0);
        throwALerror();
    }

    if(state != AL_PLAYING && state != AL_PAUSED)
    {
        ALint queued;

        alGetSourcei(mSource, AL_BUFFERS_QUEUED, &queued);
        throwALerror();
        if(queued > 0)
        {
            alSourcePlay(mSource);
            throwALerror();
        }
    }

    return !mIsFinished;
}

//
// A regular OpenAL sound
//
class OpenAL_Sound : public Sound
{
    OpenAL_Output &mOutput;

    ALuint mSource;
    ALuint mBuffer;
public:
    OpenAL_Sound(OpenAL_Output &output, ALuint src, ALuint buf);
    virtual ~OpenAL_Sound();

    virtual void stop();
    virtual bool isPlaying();
    virtual void update(const float *pos);

    static ALuint LoadBuffer(DecoderPtr decoder);
};

ALuint OpenAL_Sound::LoadBuffer(DecoderPtr decoder)
{
    int srate;
    ChannelConfig chans;
    SampleType type;
    ALenum format;

    decoder->getInfo(&srate, &chans, &type);
    format = getALFormat(chans, type);

    std::vector<char> data(32768);
    size_t got, total = 0;
    while((got=decoder->read(&data[total], data.size()-total)) > 0)
    {
        total += got;
        data.resize(total*2);
    }
    data.resize(total);

    ALuint buf=0;
    alGenBuffers(1, &buf);
    alBufferData(buf, format, &data[0], total, srate);
    return buf;
}

OpenAL_Sound::OpenAL_Sound(OpenAL_Output &output, ALuint src, ALuint buf)
  : mOutput(output), mSource(src), mBuffer(buf)
{
}
OpenAL_Sound::~OpenAL_Sound()
{
    alSourceStop(mSource);
    alSourcei(mSource, AL_BUFFER, 0);

    mOutput.mFreeSources.push_back(mSource);
    alDeleteBuffers(1, &mBuffer);
    alGetError();
}

void OpenAL_Sound::stop()
{
    alSourceStop(mSource);
    throwALerror();
}

bool OpenAL_Sound::isPlaying()
{
    ALint state;

    alGetSourcei(mSource, AL_SOURCE_STATE, &state);
    throwALerror();

    return state==AL_PLAYING;
}

void OpenAL_Sound::update(const float *pos)
{
    alSource3f(mSource, AL_POSITION, pos[0], pos[2], -pos[1]);
    alSource3f(mSource, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
    alSource3f(mSource, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    throwALerror();
}


//
// An OpenAL output device
//
void OpenAL_Output::init(const std::string &devname)
{
    if(mDevice || mContext)
        fail("Device already open");

    mDevice = alcOpenDevice(devname.c_str());
    if(!mDevice)
        fail("Failed to open \""+devname+"\"");
    std::cout << "Opened \""<<alcGetString(mDevice, ALC_DEVICE_SPECIFIER)<<"\"" << std::endl;

    mContext = alcCreateContext(mDevice, NULL);
    if(!mContext || alcMakeContextCurrent(mContext) == ALC_FALSE)
        fail(std::string("Failed to setup context: ")+alcGetString(mDevice, alcGetError(mDevice)));

    alDistanceModel(AL_LINEAR_DISTANCE_CLAMPED);
    throwALerror();

    ALCint maxmono, maxstereo;
    alcGetIntegerv(mDevice, ALC_MONO_SOURCES, 1, &maxmono);
    alcGetIntegerv(mDevice, ALC_STEREO_SOURCES, 1, &maxstereo);
    throwALCerror(mDevice);

    mFreeSources.resize(std::min(maxmono+maxstereo, 256));
    for(size_t i = 0;i < mFreeSources.size();i++)
    {
        ALuint src;
        alGenSources(1, &src);
        if(alGetError() != AL_NO_ERROR)
        {
            mFreeSources.resize(i);
            break;
        }
        mFreeSources[i] = src;
    }
    if(mFreeSources.size() == 0)
        fail("Could not allocate any sources");
}

void OpenAL_Output::deinit()
{
    mStreamThread->removeAll();

    if(!mFreeSources.empty())
    {
        alDeleteSources(mFreeSources.size(), mFreeSources.data());
        mFreeSources.clear();
    }
    alcMakeContextCurrent(0);
    if(mContext)
        alcDestroyContext(mContext);
    mContext = 0;
    if(mDevice)
        alcCloseDevice(mDevice);
    mDevice = 0;
}


Sound* OpenAL_Output::playSound(const std::string &fname, float volume, float pitch, bool loop)
{
    throwALerror();

    std::auto_ptr<OpenAL_Sound> sound;
    ALuint src=0, buf=0;

    if(mFreeSources.empty())
        fail("No free sources");
    src = mFreeSources.back();
    mFreeSources.pop_back();

    try
    {
        DecoderPtr decoder = mManager.getDecoder();
        decoder->open(fname);
        buf = OpenAL_Sound::LoadBuffer(decoder);
        throwALerror();
        decoder->close();

        sound.reset(new OpenAL_Sound(*this, src, buf));
    }
    catch(std::exception &e)
    {
        mFreeSources.push_back(src);
        if(alIsBuffer(buf))
            alDeleteBuffers(1, &buf);
        alGetError();
        throw;
    }

    alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSource3f(src, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
    alSource3f(src, AL_VELOCITY, 0.0f, 0.0f, 0.0f);

    alSourcef(src, AL_REFERENCE_DISTANCE, 1.0f);
    alSourcef(src, AL_MAX_DISTANCE, 1000.0f);
    alSourcef(src, AL_ROLLOFF_FACTOR, 0.0f);

    alSourcef(src, AL_GAIN, volume);
    alSourcef(src, AL_PITCH, pitch);

    alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcei(src, AL_LOOPING, (loop?AL_TRUE:AL_FALSE));
    throwALerror();

    alSourcei(src, AL_BUFFER, buf);
    alSourcePlay(src);
    throwALerror();

    return sound.release();
}

Sound* OpenAL_Output::playSound3D(const std::string &fname, const float *pos, float volume, float pitch,
                                  float min, float max, bool loop)
{
    throwALerror();

    std::auto_ptr<OpenAL_Sound> sound;
    ALuint src=0, buf=0;

    if(mFreeSources.empty())
        fail("No free sources");
    src = mFreeSources.back();
    mFreeSources.pop_back();

    try
    {
        DecoderPtr decoder = mManager.getDecoder();
        decoder->open(fname);
        buf = OpenAL_Sound::LoadBuffer(decoder);
        throwALerror();
        decoder->close();

        sound.reset(new OpenAL_Sound(*this, src, buf));
    }
    catch(std::exception &e)
    {
        mFreeSources.push_back(src);
        if(alIsBuffer(buf))
            alDeleteBuffers(1, &buf);
        alGetError();
        throw;
    }

    alSource3f(src, AL_POSITION, pos[0], pos[2], -pos[1]);
    alSource3f(src, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
    alSource3f(src, AL_VELOCITY, 0.0f, 0.0f, 0.0f);

    alSourcef(src, AL_REFERENCE_DISTANCE, min);
    alSourcef(src, AL_MAX_DISTANCE, max);
    alSourcef(src, AL_ROLLOFF_FACTOR, 1.0f);

    alSourcef(src, AL_GAIN, volume);
    alSourcef(src, AL_PITCH, pitch);

    alSourcei(src, AL_SOURCE_RELATIVE, AL_FALSE);
    alSourcei(src, AL_LOOPING, (loop?AL_TRUE:AL_FALSE));
    throwALerror();

    alSourcei(src, AL_BUFFER, buf);
    alSourcePlay(src);
    throwALerror();

    return sound.release();
}


Sound* OpenAL_Output::streamSound(const std::string &fname, float volume, float pitch)
{
    throwALerror();

    std::auto_ptr<OpenAL_SoundStream> sound;
    ALuint src;

    if(mFreeSources.empty())
        fail("No free sources");
    src = mFreeSources.back();
    mFreeSources.pop_back();

    try
    {
        DecoderPtr decoder = mManager.getDecoder();
        decoder->open(fname);
        sound.reset(new OpenAL_SoundStream(*this, src, decoder));
    }
    catch(std::exception &e)
    {
        mFreeSources.push_back(src);
        throw;
    }

    alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSource3f(src, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
    alSource3f(src, AL_VELOCITY, 0.0f, 0.0f, 0.0f);

    alSourcef(src, AL_REFERENCE_DISTANCE, 1.0f);
    alSourcef(src, AL_MAX_DISTANCE, 1000.0f);
    alSourcef(src, AL_ROLLOFF_FACTOR, 0.0f);

    alSourcef(src, AL_GAIN, volume);
    alSourcef(src, AL_PITCH, pitch);

    alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcei(src, AL_LOOPING, AL_FALSE);
    throwALerror();

    sound->play();
    return sound.release();
}

Sound* OpenAL_Output::streamSound3D(const std::string &fname, const float *pos, float volume, float pitch,
                                    float min, float max)
{
    throwALerror();

    std::auto_ptr<OpenAL_SoundStream> sound;
    ALuint src;

    if(mFreeSources.empty())
        fail("No free sources");
    src = mFreeSources.back();
    mFreeSources.pop_back();

    try
    {
        DecoderPtr decoder = mManager.getDecoder();
        decoder->open(fname);
        sound.reset(new OpenAL_SoundStream(*this, src, decoder));
    }
    catch(std::exception &e)
    {
        mFreeSources.push_back(src);
        throw;
    }

    alSource3f(src, AL_POSITION, pos[0], pos[2], -pos[1]);
    alSource3f(src, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
    alSource3f(src, AL_VELOCITY, 0.0f, 0.0f, 0.0f);

    alSourcef(src, AL_REFERENCE_DISTANCE, min);
    alSourcef(src, AL_MAX_DISTANCE, max);
    alSourcef(src, AL_ROLLOFF_FACTOR, 1.0f);

    alSourcef(src, AL_GAIN, volume);
    alSourcef(src, AL_PITCH, pitch);

    alSourcei(src, AL_SOURCE_RELATIVE, AL_FALSE);
    alSourcei(src, AL_LOOPING, AL_FALSE);
    throwALerror();

    sound->play();
    return sound.release();
}


void OpenAL_Output::updateListener(const float *pos, const float *atdir, const float *updir)
{
    float orient[6] = {
        atdir[0], atdir[2], -atdir[1],
        updir[0], updir[2], -updir[1]
    };

    alListener3f(AL_POSITION, pos[0], pos[2], -pos[1]);
    alListenerfv(AL_ORIENTATION, orient);
    throwALerror();
}


OpenAL_Output::OpenAL_Output(SoundManager &mgr)
  : Sound_Output(mgr), mDevice(0), mContext(0), mStreamThread(new StreamThread)
{
}

OpenAL_Output::~OpenAL_Output()
{
    deinit();
}

}
