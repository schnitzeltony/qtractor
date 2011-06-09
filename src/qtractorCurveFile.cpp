// qtractorCurveFile.cpp
//
/****************************************************************************
   Copyright (C) 2005-2011, rncbc aka Rui Nuno Capela. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

*****************************************************************************/

#include "qtractorAbout.h"
#include "qtractorCurveFile.h"

#include "qtractorDocument.h"
#include "qtractorTimeScale.h"
#include "qtractorMidiFile.h"

#include "qtractorMidiControl.h"

#include <QDomDocument>


//----------------------------------------------------------------------
// class qtractorCurveFile -- Automation curve file interface impl.
//

// Curve item list serialization methods.
bool qtractorCurveFile::save ( qtractorDocument *pDocument,
	QDomElement *pElement, qtractorTimeScale *pTimeScale ) const
{
	if (m_pCurveList == NULL)
		return false;

	unsigned short iSeq;
	unsigned short iSeqs = m_items.count();
	if (iSeqs < 1)
		return false;

	qtractorMidiFile file;
	if (!file.open(m_sFilename, qtractorMidiFile::Write))
		return false;

	unsigned short iTicksPerBeat = pTimeScale->ticksPerBeat();
	qtractorMidiSequence **ppSeqs = new qtractorMidiSequence * [iSeqs];
	for (iSeq = 0; iSeq < iSeqs; ++iSeq)
		ppSeqs[iSeq] = new qtractorMidiSequence(QString(), iSeq, iTicksPerBeat);

	iSeq = 0;

	QDomElement eItems = pDocument->document()->createElement("curve-items");
	QListIterator<Item *> iter(m_items);
	while (iter.hasNext()) {
		Item *pItem = iter.next();
		qtractorCurve *pCurve = m_pCurveList->findCurve(pItem->subject);
		if (pCurve && pCurve->isEnabled()) {
			qtractorMidiSequence *pSeq = ppSeqs[iSeq];
			pCurve->writeMidiSequence(pSeq,
				pItem->type,
				pItem->channel,
				pItem->param,
				pTimeScale);
			QDomElement eItem
				= pDocument->document()->createElement("curve-item");
			eItem.setAttribute("index",
				QString::number(pItem->index));
			eItem.setAttribute("type",
				qtractorMidiControl::textFromType(pItem->type));
			pDocument->saveTextElement("channel",
				QString::number(pItem->channel), &eItem);
			pDocument->saveTextElement("param",
				QString::number(pItem->param), &eItem);
			pDocument->saveTextElement("mode",
				textFromMode(pItem->mode), &eItem);
			pDocument->saveTextElement("process",
				pDocument->textFromBool(pItem->process), &eItem);
			pDocument->saveTextElement("capture",
				pDocument->textFromBool(pItem->capture), &eItem);
			eItems.appendChild(eItem);
		}
		++iSeq;
	}

	file.writeHeader(1, iSeqs, iTicksPerBeat);
	file.writeTracks(ppSeqs, iSeqs);
	file.close();

	for (iSeq = 0; iSeq < iSeqs; ++iSeq)
		delete ppSeqs[iSeq];
	delete [] ppSeqs;

	pDocument->saveTextElement("filename",
		pDocument->addFile(m_sFilename), pElement);
	pElement->appendChild(eItems);

	return true;
}


bool qtractorCurveFile::load ( qtractorDocument *pDocument,
	QDomElement *pElement, qtractorTimeScale */*pTimeScale*/ )
{
	clear();

	for (QDomNode nChild = pElement->firstChild();
			!nChild.isNull(); nChild = nChild.nextSibling()) {
		// Convert node to element, if any.
		QDomElement eChild = nChild.toElement();
		if (eChild.isNull())
			continue;
		// Check for child item...
		if (eChild.tagName() == "filename")
			m_sFilename = eChild.text();
		else
		if (eChild.tagName() == "curve-items") {
			for (QDomNode nItem = eChild.firstChild();
					!nItem.isNull(); nItem = nItem.nextSibling()) {
				// Convert node to element, if any.
				QDomElement eItem = nItem.toElement();
				if (eItem.isNull())
					continue;
				// Check for controller item...
				if (eItem.tagName() == "curve-item") {
					Item *pItem = new Item;
					pItem->index = eItem.attribute("index").toULong();
					pItem->type = qtractorMidiControl::typeFromText(
						eItem.attribute("type"));
					for (QDomNode nProp = eItem.firstChild();
							!nProp.isNull(); nProp = nProp.nextSibling()) {
						// Convert node to element, if any.
						QDomElement eProp = nProp.toElement();
						if (eProp.isNull())
							continue;
						// Check for property item...
						if (eProp.tagName() == "channel")
							pItem->channel = eProp.text().toUShort();
						else
						if (eProp.tagName() == "param")
							pItem->param = eProp.text().toUShort();
						else
						if (eProp.tagName() == "mode")
							pItem->mode = modeFromText(eProp.text());
						else
						if (eProp.tagName() == "process")
							pItem->process = pDocument->boolFromText(eProp.text());
						else
						if (eProp.tagName() == "capture")
							pItem->capture = pDocument->boolFromText(eProp.text());
					}
					pItem->subject = NULL;
					addItem(pItem);
				}
			}
		}
	}

	return true;
}


bool qtractorCurveFile::apply ( qtractorTimeScale *pTimeScale )
{
	if (m_pCurveList == NULL)
		return false;

	qtractorMidiFile file;
	if (!file.open(m_sFilename, qtractorMidiFile::Read))
		return false;

	unsigned short iSeq = 0;
	unsigned short iTicksPerBeat = pTimeScale->ticksPerBeat();

	QListIterator<Item *> iter(m_items);
	while (iter.hasNext()) {
		Item *pItem = iter.next();
		qtractorCurve *pCurve = m_pCurveList->findCurve(pItem->subject);
		if (pCurve == NULL)
			pCurve = new qtractorCurve(m_pCurveList, pItem->subject, pItem->mode);
		qtractorMidiSequence seq(QString(), iSeq, iTicksPerBeat);
		if (file.readTrack(&seq, iSeq)) {
			pCurve->readMidiSequence(&seq,
				pItem->type,
				pItem->channel,
				pItem->param,
				pTimeScale);
		}
		pCurve->setProcess(pItem->process);
		pCurve->setCapture(pItem->capture);
		++iSeq;
	}

	file.close();

	return true;
}


// Text/curve-mode converters...
qtractorCurve::Mode qtractorCurveFile::modeFromText ( const QString& sText )
{
	qtractorCurve::Mode mode = qtractorCurve::Hold;

	if (sText == "Spline")
		mode = qtractorCurve::Spline;
	else
	if (sText == "Linear")
		mode = qtractorCurve::Linear;

	return mode;
}


QString qtractorCurveFile::textFromMode ( qtractorCurve::Mode mode )
{
	QString sText;

	switch (mode) {
	case qtractorCurve::Spline:
		sText = "Spline";
		break;
	case qtractorCurve::Linear:
		sText = "Linear";
		break;
	case qtractorCurve::Hold:
	default:
		sText = "Hold";
		break;
	}

	return sText;
}


// end of qtractorCurveFile.cpp
