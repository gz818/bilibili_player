
#include <functional>

#include <QDebug>

#include "qvideosync.hpp"


QAudioVideoSync::QAudioVideoSync(FFPlayer* parent)
{
	m_sync_thread = std::thread(std::bind(&QAudioVideoSync::sync_thread, this));

	m_audio_out = nullptr;

	connect(this, SIGNAL(pause()), this,  SLOT(do_pause()), Qt::QueuedConnection);
	connect(this, SIGNAL(resume()), this,  SLOT(do_resume()), Qt::QueuedConnection);
}

QAudioVideoSync::~QAudioVideoSync()
{
	m_sync_thread.join();
}

void QAudioVideoSync::stop()
{
	m_stop = true;
}

void QAudioVideoSync::do_resume()
{
	if(m_audio_out)
		m_audio_out->resume();
}

void QAudioVideoSync::do_pause()
{
	if(m_audio_out)
		m_audio_out->suspend();
	play_time.stop();
}

void QAudioVideoSync::stateChanged(QAudio::State s)
{
	if (s == QAudio::SuspendedState)
	{
		suspended();
	}
	else if (s == QAudio::ActiveState)
	{
		running();
		play_time.resume();
	}
}

void QAudioVideoSync::sync_frame(const QVideoFrame& f)
{
	if (m_stop)
		return;

	QMutexLocker l(&m_lock);

	m_list.push_back(f);
}

void QAudioVideoSync::sync_audio(const QAudioBuffer& a)
{
	{
		play_time.start();

		QMutexLocker l(&m_alock);

		if(!m_audio_out)
		{
			m_audio_out = new QAudioOutput(a.format());
			m_audio_out->setNotifyInterval(2500);
			QIODevice::open(QIODevice::ReadOnly);

			// 使用 1000ms 的缓冲区
			m_audio_out->setBufferSize(a.format().sampleRate());// * (a.format().sampleSize()/8));

			m_audio_out->start(this);

			running();

			played_audio_frame_time_stamp += a.startTime();// + play_time.elapsed();
			play_time.start();
			connect(m_audio_out, SIGNAL(notify()), this, SLOT(audio_play_buffer_notify()));

			connect(m_audio_out, SIGNAL(stateChanged(QAudio::State)), this, SLOT(stateChanged(QAudio::State)));

		}

		// 挂入列队
		// 这里无需阻塞，因为视频部分会阻塞的.
		m_audiobuf_list.push_back(a);

	}
	Q_EMIT readyRead();
}

qint64 QAudioVideoSync::bytesAvailable() const
{
	QMutexLocker l(&m_alock);

	if(m_audiobuf_list.empty())
	{
		Q_EMIT need_more_frame();
		Q_EMIT nomore_frames();
		return 0;
	}

	if (m_audiobuf_list.size() < 5)
		Q_EMIT need_more_frame();

	return m_audiobuf_list.front().byteCount();

	return std::accumulate(m_audiobuf_list.begin(), m_audiobuf_list.end(), (qint64)0, [](qint64 s, const QAudioBuffer& f){
		return s + f.byteCount();
	});
}

qint64 QAudioVideoSync::writeData(const char* data, qint64 len)
{
	return 0;
}

qint64 QAudioVideoSync::readData(char* data, qint64 maxlen)
{
	QMutexLocker l(&m_alock);

	return readDataUnlocked(data, maxlen);
}

qint64 QAudioVideoSync::readDataUnlocked(char* data, qint64 maxlen)
{
	if (m_audiobuf_list.size() < 5)
		Q_EMIT need_more_frame();

	if(maxlen==0)
		return 0;

	if (m_tmp_buf_size == 0)
	{
		if(m_audiobuf_list.empty())
		{
			Q_EMIT nomore_frames();
			return 0;
		}

		QMutexLocker l(&m_ptslock);
		QAudioBuffer& f = m_audiobuf_list.front();
		// 这样视频就自动同步到声音上了
		played_audio_frame_time_stamp = f.startTime();// - 1000;


		auto audio_time_in_buffer = 1000.0 * (1.0 - ( (double) m_audio_out->bytesFree() / (double) m_audio_out->bufferSize()));
		play_time.start();

// 		played_audio_frame_time_stamp -= 1000;
// 		played_audio_frame_time_stamp -= audio_time_in_buffer;
// 		qDebug() << "now play this audioframe pts: " << played_audio_frame_time_stamp;

		m_tmp_buf = f.constData<char>();
		m_tmp_buf_size = f.byteCount();
	}

	QAudioBuffer& f = m_audiobuf_list.front();

	if (m_tmp_buf_size)
	{
		auto copy_size = qMin(maxlen, m_tmp_buf_size);

		memcpy(data, m_tmp_buf, copy_size);

		m_tmp_buf += copy_size;
		m_tmp_buf_size -= copy_size;

		if (m_tmp_buf_size==0)
		{
			m_audiobuf_list.pop_front();
		}

		return copy_size + readDataUnlocked(data + copy_size, maxlen - copy_size);
	}

}

void QAudioVideoSync::audio_play_buffer_notify()
{
	m_avsync_notify.wakeAll();
}

void QAudioVideoSync::sync_thread()
{
	QVideoFrame f;

	for(;!m_stop;)
	{
		// 读取

		{
			QMutexLocker l(&m_lock);

			while(m_list.empty())
			{
				Q_EMIT need_more_frame();
				l.unlock();
				Q_EMIT nomore_frames();
				QThread::msleep(500);
				l.relock();
			}

			f = m_list.front();
			m_list.pop_front();

			if (m_list.size() < 5)
				Q_EMIT need_more_frame();
		}

		{
			QMutexLocker l(&m_alock);
			if (m_audiobuf_list.size() > 5)
				Q_EMIT frames_ready();
		}

		double base_shift = 0.0;

		// 接着同步到时间点
		{

		QMutexLocker l(&m_ptslock);

		auto elapsed = played_audio_frame_time_stamp + play_time.elapsed().wall/1000000 + base_shift;

// 		qDebug() << "now play this videoframe pts: " << f.startTime() << " (" << elapsed << ") ";

		auto startTime = f.startTime();

		while ( elapsed < (startTime - 1))
		{
			l.unlock();
			QThread::msleep(5);
			l.relock();

			elapsed = played_audio_frame_time_stamp + play_time.elapsed().wall/1000000 + base_shift;
		}
		}

		render_frame(f);

	}

	m_lock.lock();
	m_list.clear();
	m_lock.unlock();
	return;
}
