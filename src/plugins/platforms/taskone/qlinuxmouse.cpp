/****************************************************************************
**
** Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
** Copyright (C) 2012 TaskOne
** All rights reserved.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Palm gives you certain additional
** rights.  These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qlinuxmouse.h"

#include <Qt>
#include <QKeyEvent>
#include <QApplication>
#include <QSocketNotifier>
#include <QStringList>
#include <QWindowSystemInterface>

#include <qplatformdefs.h>
#include <private/qcore_unix_p.h>

#include <errno.h>
#include <linux/input.h>

#include <qdebug.h>

#define TASKONE_SCREEN_WIDTH  1920
#define TASKONE_SCREEN_HEIGHT 1080

QLinuxMouseHandler::QLinuxMouseHandler(const QString &specification)
    : m_notify(0), m_x(0), m_y(0), m_prevx(0), m_prevy(0), m_xoffset(0), m_yoffset(0), m_buttons(0), d(0)
{
    qDebug() << "QLinuxMouseHandler" << specification;

    setObjectName(QLatin1String("LinuxInputSubsystem Mouse Handler"));

    QString dev = QLatin1String("/dev/input/event0");
    m_compression = true;
    m_smooth = false;
    int jitterLimit = 0;
    
    QStringList args = specification.split(QLatin1Char(':'));
    foreach (const QString &arg, args) {
        if (arg == "nocompress")
            m_compression = false;
        else if (arg.startsWith("dejitter="))
            jitterLimit = arg.mid(9).toInt();
        else if (arg.startsWith("xoffset="))
            m_xoffset = arg.mid(8).toInt();
        else if (arg.startsWith("yoffset="))
            m_yoffset = arg.mid(8).toInt();
        else if (arg.startsWith(QLatin1String("/dev/")))
            dev = arg;
    }
    m_jitterLimitSquared = jitterLimit*jitterLimit; 
    
    m_fd = QT_OPEN(dev.toLocal8Bit().constData(), O_RDONLY | O_NDELAY, 0);
    if (m_fd >= 0) {
        m_notify = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notify, SIGNAL(activated(int)), this, SLOT(readMouseData()));
    } else {
        qWarning("Cannot open mouse input device '%s': %s", qPrintable(dev), strerror(errno));
        return;
    }
}

QLinuxMouseHandler::~QLinuxMouseHandler()
{
    if (m_fd >= 0)
        QT_CLOSE(m_fd);
}

void QLinuxMouseHandler::sendMouseEvent(int x, int y, Qt::MouseButtons buttons)
{
    QPoint pos(x+m_xoffset, y+m_yoffset);
    QWindowSystemInterface::handleMouseEvent(QApplication::activeWindow(), pos, pos, buttons);
    m_prevx = x;
    m_prevy = y;
}

void QLinuxMouseHandler::readMouseData()
{
    struct ::input_event buffer[32];
    int n = 0;
    bool posChanged = false;
    bool pendingMouseEvent = false;
    int eventCompressCount = 0;
    forever {
        n = QT_READ(m_fd, reinterpret_cast<char *>(buffer) + n, sizeof(buffer) - n);

        if (n == 0) {
            qWarning("Got EOF from the input device.");
            return;
        } else if (n < 0 && (errno != EINTR && errno != EAGAIN)) {
            qWarning("Could not read from input device: %s", strerror(errno));
            return;
        } else if (n % sizeof(buffer[0]) == 0) {
            break;
        }
    }

    n /= sizeof(buffer[0]);

    for (int i = 0; i < n; ++i) {
        struct ::input_event *data = &buffer[i];
        bool unknown = false;
        if (data->type == EV_ABS) {
            if (data->code == ABS_X && m_x != data->value) {
                m_x = data->value;
                posChanged = true;
            } else if (data->code == ABS_Y && m_y != data->value) {
                m_y = data->value;
                posChanged = true;
            } else if (data->code == ABS_PRESSURE) {
                //ignore for now...
            } else if (data->code == ABS_TOOL_WIDTH) {
                //ignore for now...
            } else if (data->code == ABS_HAT0X) {
                //ignore for now...
            } else if (data->code == ABS_HAT0Y) {
                //ignore for now...
            } else {
                unknown = true;
            }
        } else if (data->type == EV_REL) {
            if (data->code == REL_X) {
                m_x += data->value;
                posChanged = true;
            } else if (data->code == REL_Y) {
                m_y += data->value;
                posChanged = true;
            }
        } else if (data->type == EV_KEY && data->code == BTN_TOUCH) {
            m_buttons = data->value ? Qt::LeftButton : Qt::NoButton;

            sendMouseEvent(m_x, m_y, m_buttons);
            pendingMouseEvent = false;
        } else if (data->type == EV_KEY && data->code >= BTN_LEFT && data->code <= BTN_MIDDLE) {
            Qt::MouseButton button = Qt::NoButton;
            switch (data->code) {
            case BTN_LEFT: button = Qt::LeftButton; break;
            case BTN_MIDDLE: button = Qt::MidButton; break;
            case BTN_RIGHT: button = Qt::RightButton; break;
            }
            if (data->value)
                m_buttons |= button;
            else
                m_buttons &= ~button;
            sendMouseEvent(m_x, m_y, m_buttons);
            pendingMouseEvent = false;
        } else if (data->type == EV_SYN && data->code == SYN_REPORT) {
            if (posChanged) {
                // Saturation of position
                if( m_x < 0 )
                {
                    m_x = 0;
                }
                if( m_y < 0 )
                {
                    m_y = 0;
                }
                if( m_x > TASKONE_SCREEN_WIDTH )
                {
                    m_x = TASKONE_SCREEN_WIDTH;
                }
                if( m_y > TASKONE_SCREEN_HEIGHT )
                {
                    m_y = TASKONE_SCREEN_HEIGHT;
                }

                posChanged = false;
                if (m_compression) {
                    pendingMouseEvent = true;
                    eventCompressCount++;
                } else {
                    sendMouseEvent(m_x, m_y, m_buttons);
                }
            }
        } else if (data->type == EV_MSC && data->code == MSC_SCAN) {
            // kernel encountered an unmapped key - just ignore it
            continue;
        } else {
            unknown = true;
        }
#ifdef QLINUXINPUT_EXTRA_DEBUG
        if (unknown) {
            qWarning("unknown mouse event type=%x, code=%x, value=%x", data->type, data->code, data->value);
        }
#endif        
    }
    if (m_compression && pendingMouseEvent) {
        int distanceSquared = (m_x - m_prevx)*(m_x - m_prevx) + (m_y - m_prevy)*(m_y - m_prevy);
        if (distanceSquared > m_jitterLimitSquared)
            sendMouseEvent(m_x, m_y, m_buttons);
    }
}


