// qtractorMidiEngine.cpp
//
/****************************************************************************
   Copyright (C) 2005-2006, rncbc aka Rui Nuno Capela. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

*****************************************************************************/

#include "qtractorAbout.h"
#include "qtractorMidiEngine.h"
#include "qtractorMidiEvent.h"

#include "qtractorSessionCursor.h"
#include "qtractorSessionDocument.h"
#include "qtractorAudioEngine.h"
#include "qtractorClip.h"

#include <qthread.h>


//----------------------------------------------------------------------
// class qtractorMidiOutputThread -- MIDI output thread (singleton).
//

class qtractorMidiOutputThread : public QThread
{
public:

	// Constructor.
	qtractorMidiOutputThread(qtractorSession *pSession,
		unsigned int iReadAhead = 0);

	// Destructor.
	~qtractorMidiOutputThread();

	// Thread run state accessors.
	void setRunState(bool bRunState);
	bool runState() const;

	// Read ahead frames configuration.
	void setReadAhead(unsigned int iReadAhead);
	unsigned int readAhead() const;

	// MIDI/Audio sync-check predicate.
	qtractorSessionCursor *midiCursorSync(bool bStart = false);

	// MIDI track output process resync.
	void trackSync(qtractorTrack *pTrack, unsigned long iFrameStart);

	// MIDI output process cycle iteration (locked).
	void processSync();

	// Wake from executive wait condition.
	void sync();

protected:

	// The main thread executive.
	void run();

	// MIDI output process cycle iteration.
	void process();

private:

	// The thread launcher engine.
	qtractorSession *m_pSession;

	// State variables.
	unsigned long m_iFrame;
	unsigned int  m_iReadAhead;

	// Whether the thread is logically running.
	bool m_bRunState;

	// Thread synchronization objects.
	QMutex m_mutex;
	QWaitCondition m_cond;
};


//----------------------------------------------------------------------
// class qtractorMidiOutputThread -- MIDI output thread (singleton).
//

// Constructor.
qtractorMidiOutputThread::qtractorMidiOutputThread (
	qtractorSession *pSession, unsigned int iReadAhead ) : QThread()
{
	if (iReadAhead == 0)
		iReadAhead = pSession->sampleRate();

	m_pSession   = pSession;
	m_bRunState  = false;
	m_iFrame     = 0;
	m_iReadAhead = iReadAhead;
}


// Destructor.
qtractorMidiOutputThread::~qtractorMidiOutputThread (void)
{
	// Try to wake and terminate executive thread.
	if (runState())
		setRunState(false);
	// Give it a bit of time to cleanup...
	if (running()) {
		sync();
		QThread::msleep(100);
	}
}


// Thread run state accessors.
void qtractorMidiOutputThread::setRunState ( bool bRunState )
{
	m_bRunState = bRunState;
}

bool qtractorMidiOutputThread::runState (void) const
{
	return m_bRunState;
}


// Read ahead frames configuration.
void qtractorMidiOutputThread::setReadAhead ( unsigned int iReadAhead )
{
	m_iReadAhead = iReadAhead;
}

unsigned int qtractorMidiOutputThread::readAhead (void) const
{
	return m_iReadAhead;
}


// Audio/MIDI sync-check and cursor predicate.
qtractorSessionCursor *qtractorMidiOutputThread::midiCursorSync ( bool bStart )
{
	// We'll need access to master audio engine...
	qtractorSessionCursor *pAudioCursor
		= m_pSession->audioEngine()->sessionCursor();
	if (pAudioCursor == NULL)
		return NULL;

	// And to our slave MIDI engine too...
	qtractorSessionCursor *pMidiCursor
		= m_pSession->midiEngine()->sessionCursor();
	if (pMidiCursor == NULL)
		return NULL;

	// Can MIDI be ever behind audio?
	if (bStart)
		pMidiCursor->seek(pAudioCursor->frame());
	else if (pMidiCursor->frame() > pAudioCursor->frame() + m_iReadAhead)
		return NULL;

	// Nope. OK.
	return pMidiCursor;
}


// The main thread executive.
void qtractorMidiOutputThread::run (void)
{
#ifdef DEBUG
	fprintf(stderr, "qtractorMidiOutputThread::run(%p): started.\n", this);
#endif

	m_bRunState = true;

	m_mutex.lock();
	while (m_bRunState) {
		// Wait for sync...
		m_cond.wait(&m_mutex);
#ifdef DEBUG
		fprintf(stderr, "qtractorMidiOutputThread::run(%p): waked.\n", this);
#endif
		// Only if playing, the output process cycle.
		if (m_pSession->isPlaying())
			process();
	}
	m_mutex.unlock();

#ifdef DEBUG
	fprintf(stderr, "qtractorMidiOutputThread::run(%p): stopped.\n", this);
#endif
}


// MIDI output process cycle iteration.
void qtractorMidiOutputThread::process (void)
{
	// Get a handle on our slave MIDI engine...
	qtractorSessionCursor *pMidiCursor = midiCursorSync();
	// IS MIDI slightly behind audio?
	if (pMidiCursor) {
		// Now for the next readahead bunch...
		unsigned long iFrameStart = pMidiCursor->frame();
		unsigned long iFrameEnd   = iFrameStart + m_iReadAhead;
#ifdef DEBUG
		fprintf(stderr, "qtractorMidiOutputThread::process(%p, %lu, %lu)\n",
			this, iFrameStart, iFrameEnd);
#endif
		// For every MIDI track...
		int iTrack = 0;
		qtractorTrack *pTrack = m_pSession->tracks().first();
		while (pTrack) {
			if (pTrack->trackType() == qtractorTrack::Midi) {
				pTrack->process(pMidiCursor->clip(iTrack),
					iFrameStart, iFrameEnd);
			}
			pTrack = pTrack->next();
			iTrack++;
		}
		// Flush the MIDI engine output queue...
		m_pSession->midiEngine()->flush();
		// Sync to last bunch.
		pMidiCursor->seek(iFrameEnd);
	}
}


// MIDI output process cycle iteration (locked).
void qtractorMidiOutputThread::processSync (void)
{
	QMutexLocker locker(&m_mutex);
#ifdef DEBUG
	fprintf(stderr, "qtractorMidiOutputThread::processSync(%p)\n", this);
#endif
	process();
}


// MIDI track output process resync.
void qtractorMidiOutputThread::trackSync ( qtractorTrack *pTrack,
	unsigned long iFrameStart )
{
	QMutexLocker locker(&m_mutex);

	// Pick our actual MIDI sequencer cursor...
	qtractorSessionCursor *pMidiCursor
		= m_pSession->midiEngine()->sessionCursor();
	if (pMidiCursor == NULL)
		return;

	// This is the last framestamp to be trown out...
	unsigned long iFrameEnd = pMidiCursor->frame();

#ifdef DEBUG
	fprintf(stderr, "qtractorMidiOutputThread::trackSync(%p, %lu, %lu)\n",
		this, iFrameStart, iFrameEnd);
#endif

	// Locate the immediate nearest clip in track
	// and render them all thereafter, immediately...
	qtractorClip *pClip = pTrack->clips().first();
	while (pClip && pClip->clipStart() < iFrameEnd) {
		if (iFrameStart < pClip->clipStart() + pClip->clipLength())
			pClip->process(1.0, iFrameStart, iFrameEnd);
		pClip = pClip->next();
	}

	// Surely if must realize the output queue...
	m_pSession->midiEngine()->flush();
}


// Wake from executive wait condition.
void qtractorMidiOutputThread::sync (void)
{
	if (m_mutex.tryLock()) {
		m_cond.wakeAll();
		m_mutex.unlock();
	}
#ifdef DEBUG_0
	else fprintf(stderr, "qtractorMidiOutputThread::sync(%p): tryLock() failed.\n", this);
#endif
}


//----------------------------------------------------------------------
// class qtractorMidiEngine -- ALSA sequencer client instance (singleton).
//

// Constructor.
qtractorMidiEngine::qtractorMidiEngine ( qtractorSession *pSession )
	: qtractorEngine(pSession, qtractorTrack::Midi)
{
	m_pAlsaSeq    = NULL;
	m_iAlsaClient = 0;
	m_iAlsaQueue  = 0;
	
	m_pOutputThread = NULL;
	m_iTimeStart = 0;
}


// ALSA sequencer client descriptor accessor.
snd_seq_t *qtractorMidiEngine::alsaSeq (void) const
{
	return m_pAlsaSeq;
}

int qtractorMidiEngine::alsaClient (void) const
{
	return m_iAlsaClient;
}

int qtractorMidiEngine::alsaQueue (void) const
{
	return m_iAlsaQueue;
}


// Special slave sync method.
void qtractorMidiEngine::sync (void)
{
	// Pure conditional thread slave syncronization...
	if (m_pOutputThread && m_pOutputThread->midiCursorSync())
		m_pOutputThread->sync();
}


// MIDI event enqueue method.
void qtractorMidiEngine::enqueue ( qtractorTrack *pTrack,
	qtractorMidiEvent *pEvent, unsigned long iTime, float fGain )
{
	// Target MIDI bus...
	qtractorMidiBus *pMidiBus
		= static_cast<qtractorMidiBus *> (pTrack->bus());
	if (pMidiBus == NULL)
		return;

	// Initializing...
	snd_seq_event_t ev;
	snd_seq_ev_clear(&ev);

	// Set Event tag...
	ev.tag = (unsigned char) (pTrack->midiTag() & 0xff);

	// Addressing...
	snd_seq_ev_set_source(&ev, pMidiBus->alsaPort());
	snd_seq_ev_set_subs(&ev);

	// Scheduled delivery: take into account
	// the time playback/queue started...
	snd_seq_ev_schedule_tick(&ev, m_iAlsaQueue, 0, iTime - m_iTimeStart);

	// Set proper event data...
	switch (pEvent->type()) {
		case qtractorMidiEvent::NOTEON:
			ev.type = SND_SEQ_EVENT_NOTE;
			ev.data.note.channel    = pTrack->midiChannel();
			ev.data.note.note       = pEvent->note();
			ev.data.note.velocity   = int(fGain * float(pEvent->velocity()));
			ev.data.note.duration   = pEvent->duration();
			break;
		case qtractorMidiEvent::KEYPRESS:
			ev.type = SND_SEQ_EVENT_KEYPRESS;
			ev.data.control.channel = pTrack->midiChannel();
			ev.data.control.param   = pEvent->note();
			ev.data.control.value   = pEvent->value();
			break;
		case qtractorMidiEvent::CONTROLLER:
			ev.type = SND_SEQ_EVENT_CONTROLLER;
			ev.data.control.channel = pTrack->midiChannel();
			ev.data.control.param   = pEvent->controller();
			ev.data.control.value   = pEvent->value();
			break;
		case qtractorMidiEvent::PGMCHANGE:
			ev.type = SND_SEQ_EVENT_PGMCHANGE;
			ev.data.control.channel = pTrack->midiChannel();
			ev.data.control.value   = pEvent->value();
			break;
		case qtractorMidiEvent::CHANPRESS:
			ev.type = SND_SEQ_EVENT_CHANPRESS;
			ev.data.control.channel = pTrack->midiChannel();
			ev.data.control.value   = pEvent->value();
			break;
		case qtractorMidiEvent::PITCHBEND:
			ev.type = SND_SEQ_EVENT_PITCHBEND;
			ev.data.control.channel = pTrack->midiChannel();
			ev.data.control.value   = pEvent->value();
			break;
		case qtractorMidiEvent::SYSEX: {
			ev.type = SND_SEQ_EVENT_SYSEX;
			unsigned int   iSysex = pEvent->sysex_len() + 2;
			unsigned char *pSysex = new unsigned char [iSysex];
			pSysex[0] = 0xf0;
			::memcpy(&pSysex[1], pEvent->sysex(), pEvent->sysex_len());
			pSysex[iSysex - 1] = 0xf7;
			snd_seq_ev_set_sysex(&ev, iSysex, pSysex);
			break;
		}
		default:
			break;
	}

	// Pump it into the queue.
	snd_seq_event_output(m_pAlsaSeq, &ev);
}


// Flush ouput queue (if necessary)...
void qtractorMidiEngine::flush (void)
{
	snd_seq_drain_output(m_pAlsaSeq);
}


// Device engine initialization method.
bool qtractorMidiEngine::init ( const QString& sClientName )
{
	// Try open a new client...
	if (snd_seq_open(&m_pAlsaSeq, "hw", SND_SEQ_OPEN_DUPLEX, 0) < 0)
		return false;

	// Fix client name.
	snd_seq_set_client_name(m_pAlsaSeq, sClientName.latin1());

	m_iAlsaClient = snd_seq_client_id(m_pAlsaSeq);
	m_iAlsaQueue  = snd_seq_alloc_queue(m_pAlsaSeq);

	return true;
}


// Device engine activation method.
bool qtractorMidiEngine::activate (void)
{
	// create and start our own MIDI output queue thread...
	m_pOutputThread = new qtractorMidiOutputThread(session());
	m_pOutputThread->start(QThread::HighPriority);
	m_iTimeStart = 0;

	return true;
}


// Device engine start method.
bool qtractorMidiEngine::start (void)
{
	if (!isActivated())
		return false;

	// There must a session reference...
	qtractorSession *pSession = session();
	if (pSession == NULL)
		return false;
	// ouput thread must be around too...
	if (m_pOutputThread == NULL)
		return false;

	// Set queue tempo...
	snd_seq_queue_tempo_t *tempo;
	snd_seq_queue_tempo_alloca(&tempo);
	// Fill tempo struct with current tempo info.
	snd_seq_get_queue_tempo(m_pAlsaSeq, m_iAlsaQueue, tempo);
	// Set the new intended ones...
	snd_seq_queue_tempo_set_ppq(tempo, (int) pSession->ticksPerBeat());
	snd_seq_queue_tempo_set_tempo(tempo,
		(unsigned int) (60000000.0 / pSession->tempo()));
	// give tempo struct to the queue.
	snd_seq_set_queue_tempo(m_pAlsaSeq, m_iAlsaQueue, tempo);

	// Initial output thread bumping...
	qtractorSessionCursor *pMidiCursor
		= m_pOutputThread->midiCursorSync(true);
	if (pMidiCursor == NULL)
		return false;

	// Start queue timer...
	m_iTimeStart = pSession->tickFromFrame(pMidiCursor->frame());
	snd_seq_start_queue(m_pAlsaSeq, m_iAlsaQueue, NULL);

	// We're now ready and running...
	m_pOutputThread->processSync();

	return true;
}


// Device engine stop method.
void qtractorMidiEngine::stop (void)
{
	if (!isActivated())
		return;

	// Cleanup queue...
	snd_seq_drop_output(m_pAlsaSeq);
	// Stop queue timer...
	snd_seq_stop_queue(m_pAlsaSeq, m_iAlsaQueue, NULL);
}


// Device engine deactivation method.
void qtractorMidiEngine::deactivate (void)
{
	// We're stopping now...
	setPlaying(false);

	// Stop our output thread...
	m_pOutputThread->setRunState(false);
	m_pOutputThread->sync();
}


// Device engine cleanup method.
void qtractorMidiEngine::clean (void)
{
	// Delete output thread...
	if (m_pOutputThread) {
		delete m_pOutputThread;
		m_pOutputThread = NULL;
		m_iTimeStart = 0;
	}

	// Drop everything else, finally.
	if (m_pAlsaSeq) {
		snd_seq_free_queue(m_pAlsaSeq, m_iAlsaQueue);
		snd_seq_close(m_pAlsaSeq);
		m_iAlsaQueue  = 0;
		m_iAlsaClient = 0;
		m_pAlsaSeq = NULL;
	}
}


// Immediate track mute.
void qtractorMidiEngine::trackMute ( qtractorTrack *pTrack, bool bMute )
{
#ifdef CONFIG_DEBUG
	fprintf(stderr, "qtractorMidiEngine::trackMute(%p, %d)\n", pTrack, bMute);
#endif

	unsigned long iFrame = session()->playHead();

	if (bMute) {
		// Remove all already enqueued events
		// for the given track and channel...
		snd_seq_remove_events_t *pre;
		snd_seq_remove_events_alloca(&pre);
		snd_seq_timestamp_t ts;
		unsigned long iTime = session()->tickFromFrame(iFrame);
		ts.tick = (iTime > m_iTimeStart ? iTime - m_iTimeStart : 0);
		snd_seq_remove_events_set_time(pre, &ts);
		snd_seq_remove_events_set_tag(pre, pTrack->midiTag());
		snd_seq_remove_events_set_channel(pre, pTrack->midiChannel());
		snd_seq_remove_events_set_queue(pre, m_iAlsaQueue);
		snd_seq_remove_events_set_condition(pre, SND_SEQ_REMOVE_OUTPUT
			| SND_SEQ_REMOVE_TIME_AFTER | SND_SEQ_REMOVE_TIME_TICK
			| SND_SEQ_REMOVE_DEST_CHANNEL | SND_SEQ_REMOVE_IGNORE_OFF
			| SND_SEQ_REMOVE_TAG_MATCH);
		snd_seq_remove_events(m_pAlsaSeq, pre);
		// Done mute.
	} else {
		// Must redirect to MIDI ouput thread:
		// the immediate re-enqueueing of MIDI events.
		m_pOutputThread->trackSync(pTrack, iFrame);
		// Done unmute.
	}
}


// Document element methods.
bool qtractorMidiEngine::loadElement ( qtractorSessionDocument *pDocument,
	QDomElement *pElement )
{
	qtractorEngine::clear();

	// Load session children...
	for (QDomNode nChild = pElement->firstChild();
			!nChild.isNull();
				nChild = nChild.nextSibling()) {

		// Convert node to element...
		QDomElement eChild = nChild.toElement();
		if (eChild.isNull())
			continue;

		if (eChild.tagName() == "midi-bus") {
			QString sBusName = eChild.attribute("name");
			qtractorMidiBus::BusMode busMode
				= pDocument->loadBusMode(eChild.attribute("mode"));
			qtractorMidiBus *pMidiBus
				= new qtractorMidiBus(sBusName, busMode);
			qtractorMidiEngine::addBus(pMidiBus);
			// Load bus properties...
			for (QDomNode nProp = eChild.firstChild();
					!nProp.isNull();
						nProp = nProp.nextSibling()) {
				// Convert node to element...
				QDomElement eProp = nProp.toElement();
				if (eProp.isNull())
					continue;
				// Load map elements (non-critical)...
				if (eProp.tagName() == "midi-map")
					pMidiBus->loadElement(pDocument, &eProp);
			}
		}
	}

	return true;
}


bool qtractorMidiEngine::saveElement ( qtractorSessionDocument *pDocument,
	QDomElement *pElement )
{
	// Save audio busses...
	for (qtractorBus *pBus = qtractorEngine::busses().first();
			pBus; pBus = pBus->next()) {
		qtractorMidiBus *pMidiBus
			= static_cast<qtractorMidiBus *> (pBus);
		if (pMidiBus) {
			// Create the new audio bus element...
			QDomElement eMidiBus
				= pDocument->document()->createElement("midi-bus");
			eMidiBus.setAttribute("name",
				pMidiBus->busName());
			eMidiBus.setAttribute("mode",
				pDocument->saveBusMode(pMidiBus->busMode()));
			// Create the map element...
			QDomElement eMidiMap
				= pDocument->document()->createElement("midi-map");
			pMidiBus->saveElement(pDocument, &eMidiMap);
			// Add this clip...
			eMidiBus.appendChild(eMidiMap);
			pElement->appendChild(eMidiBus);
		}
	}

	return true;
}


//----------------------------------------------------------------------
// class qtractorMidiBus -- Managed ALSA sequencer port set
//

// Constructor.
qtractorMidiBus::qtractorMidiBus ( const QString& sBusName,
	BusMode mode ) : qtractorBus(sBusName, mode)
{
	m_iAlsaPort = 0;
}


// ALSA sequencer port accessor.
int qtractorMidiBus::alsaPort (void) const
{
	return m_iAlsaPort;
}


// Register and pre-allocate bus port buffers.
bool qtractorMidiBus::open (void)
{
	close();

	qtractorMidiEngine *pMidiEngine
		= static_cast<qtractorMidiEngine *> (engine());
	if (pMidiEngine == NULL)
		return false;
	if (pMidiEngine->alsaSeq() == NULL)
		return false;

	unsigned int flags = 0;
	
	if (busMode() & qtractorBus::Input)
		flags |= SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;

	if (busMode() & qtractorBus::Output)
		flags |= SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;

	m_iAlsaPort = snd_seq_create_simple_port(
		pMidiEngine->alsaSeq(), busName().latin1(), flags,
		SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);

	return (m_iAlsaPort >= 0);
}


// Unregister and post-free bus port buffers.
void qtractorMidiBus::close (void)
{
	// WTF?...
}


// Direct MIDI bank/program selection helper.
void qtractorMidiBus::setPatch ( unsigned short iChannel,
	const QString& sInstrumentName, int iBank, int iProg,
	int iBankSelMethod )
{
	// We always need our MIDI engine refrence...
	qtractorMidiEngine *pMidiEngine
		= static_cast<qtractorMidiEngine *> (engine());
	if (pMidiEngine == NULL)
		return;

#ifdef CONFIG_DEBUG
	fprintf(stderr, "qtractorMidiBus::setPatch(%d, \"%s\", %d, %d, %d)\n",
		iChannel, sInstrumentName.latin1(), iBank, iProg, iBankSelMethod);
#endif

	// Update patch mapping...
	if (!sInstrumentName.isEmpty())
		m_map[iChannel & 0x0f] = sInstrumentName;

	// Don't do anything else if engine
	// has not been activated...
	if (pMidiEngine->alsaSeq() == NULL)
		return;

	// Initialize sequencer event...
	snd_seq_event_t ev;
	snd_seq_ev_clear(&ev);

	// Addressing...
	snd_seq_ev_set_source(&ev, m_iAlsaPort);
	snd_seq_ev_set_subs(&ev);

	// The event will be direct...
	snd_seq_ev_set_direct(&ev);

	// Select Bank MSB.
	if (iBank >= 0 && (iBankSelMethod == 0 || iBankSelMethod == 1)) {
		ev.type = SND_SEQ_EVENT_CONTROLLER;
		ev.data.control.channel = iChannel;
		ev.data.control.param   = 0x00;		// Select Bank MSB.
		ev.data.control.value   = (iBank & 0x3f80) >> 7;
		snd_seq_event_output(pMidiEngine->alsaSeq(), &ev);
	}

	// Select Bank LSB.
	if (iBank >= 0 && (iBankSelMethod == 0 || iBankSelMethod == 2)) {
		ev.type = SND_SEQ_EVENT_CONTROLLER;
		ev.data.control.channel = iChannel;
		ev.data.control.param   = 0x20;		// Select Bank LSB.
		ev.data.control.value   = (iBank & 0x007f);
		snd_seq_event_output(pMidiEngine->alsaSeq(), &ev);
	}

	// Program change...
	ev.type = SND_SEQ_EVENT_PGMCHANGE;
	ev.data.control.channel = iChannel;
	ev.data.control.value   = iProg;
	snd_seq_event_output(pMidiEngine->alsaSeq(), &ev);

	pMidiEngine->flush();
}


// Document element methods.
bool qtractorMidiBus::loadElement ( qtractorSessionDocument * /* pDocument */,
	QDomElement *pElement )
{
	m_map.clear();

	// Load map items...
	for (QDomNode nChild = pElement->firstChild();
			!nChild.isNull();
				nChild = nChild.nextSibling()) {

		// Convert node to element...
		QDomElement eChild = nChild.toElement();
		if (eChild.isNull())
			continue;

		// Load (other) track properties..
		if (eChild.tagName() == "midi-patch") {
			unsigned short iChannel = eChild.attribute("channel").toUShort();
			for (QDomNode nPatch = eChild.firstChild();
					!nPatch.isNull();
						nPatch = nPatch.nextSibling()) {
				// Convert patch node to element...
				QDomElement ePatch = nPatch.toElement();
				if (ePatch.isNull())
					continue;
				// Add this one to map...
				if (ePatch.tagName() == "midi-instrument")
					m_map[iChannel & 0x0f] = ePatch.text();
			}
		}
	}

	return true;
}


bool qtractorMidiBus::saveElement ( qtractorSessionDocument *pDocument,
	QDomElement *pElement )
{
	// Save map items...
	QMap<int, QString>::Iterator iter;
	for (iter = m_map.begin(); iter != m_map.end(); ++iter) {
		const QString& sInstrumentName = iter.data();
		QDomElement ePatch = pDocument->document()->createElement("midi-patch");
		ePatch.setAttribute("channel", QString::number(iter.key()));
		if (!sInstrumentName.isEmpty()) {
			pDocument->saveTextElement("midi-instrument",
				sInstrumentName, &ePatch);
		}
		pElement->appendChild(ePatch);
	}

	return true;
}


// end of qtractorMidiEngine.cpp
