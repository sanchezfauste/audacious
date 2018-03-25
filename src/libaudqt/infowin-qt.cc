/*
 * infowin.cc
 * Copyright 2006-2014 William Pitcock, Tomasz Moń, Eugene Zagidullin,
 *                     John Lindgren, and Thomas Lange
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include <math.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPainter>
#include <QPushButton>
#include <QTextDocument>
#include <QVBoxLayout>

#include <libaudcore/audstrings.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/interface.h>
#include <libaudcore/playlist.h>
#include <libaudcore/probe.h>

#include "info-widget.h"
#include "libaudqt.h"
#include "libaudqt-internal.h"

namespace audqt {

/* This class remedies some of the deficiencies of QLabel (such as lack
 * of proper wrapping).  It can be expanded and/or made more visible if
 * it turns out to be useful outside InfoWindow. */
class TextWidget : public QWidget
{
public:
    TextWidget ()
    {
        m_doc.setDefaultFont (font ());
    }

    void setText (const QString & text)
    {
        m_doc.setPlainText (text);
        updateGeometry ();
    }

    void setWidth (int width)
    {
        m_doc.setTextWidth (width);
        updateGeometry ();
    }

protected:
    QSize sizeHint () const override
    {
        qreal width = m_doc.idealWidth ();
        qreal height = m_doc.size ().height ();
        return QSize (ceil (width), ceil (height));
    }

    QSize minimumSizeHint () const override
        { return sizeHint (); }

    void changeEvent (QEvent * event) override
    {
        if (event->type () == QEvent::FontChange)
        {
            m_doc.setDefaultFont (font ());
            updateGeometry ();
        }
    }

    void paintEvent (QPaintEvent * event) override
    {
        QPainter painter (this);
        m_doc.drawContents (& painter);
    }

private:
    QTextDocument m_doc;
};

class InfoWindow : public QDialog
{
public:
    InfoWindow (QWidget * parent = nullptr);

    void fillInfo (const char * filename, const Tuple & tuple,
     PluginHandle * decoder, bool updating_enabled);

private:
    String m_filename;
    QLabel m_image;
    TextWidget m_uri_label;
    InfoWidget m_infowidget;

    void displayImage (const char * filename);

    const HookReceiver<InfoWindow, const char *>
     art_hook {"art ready", this, & InfoWindow::displayImage};
};

InfoWindow::InfoWindow (QWidget * parent) : QDialog (parent)
{
    setWindowTitle (_("Song Info"));
    setContentsMargins (margins.TwoPt);

    m_image.setAlignment (Qt::AlignCenter);
    m_uri_label.setWidth (2 * audqt::sizes.OneInch);
    m_uri_label.setContextMenuPolicy (Qt::CustomContextMenu);

    connect (& m_uri_label, & QWidget::customContextMenuRequested, [this] (const QPoint & pos) {
        show_copy_context_menu (this, m_uri_label.mapToGlobal (pos), QString (m_filename));
    });

    auto left_vbox = make_vbox (nullptr);
    left_vbox->addWidget (& m_image);
    left_vbox->addWidget (& m_uri_label);
    left_vbox->setStretch (0, 1);
    left_vbox->setStretch (1, 0);

    auto hbox = make_hbox (nullptr);
    hbox->addLayout (left_vbox);
    hbox->addWidget (& m_infowidget);

    auto vbox = make_vbox (this);
    vbox->addLayout (hbox);

    auto bbox = new QDialogButtonBox (QDialogButtonBox::Save | QDialogButtonBox::Close, this);
    bbox->button (QDialogButtonBox::Save)->setText (translate_str (N_("_Save")));
    bbox->button (QDialogButtonBox::Close)->setText (translate_str (N_("_Close")));
    vbox->addWidget (bbox);

    connect (bbox, & QDialogButtonBox::accepted, [this] () {
        m_infowidget.updateFile ();
        deleteLater ();
    });

    connect (bbox, & QDialogButtonBox::rejected, this, & QObject::deleteLater);
}

void InfoWindow::fillInfo (const char * filename, const Tuple & tuple,
 PluginHandle * decoder, bool updating_enabled)
{
    m_filename = String (filename);
    m_uri_label.setText ((QString) uri_to_display (filename));
    displayImage (filename);
    m_infowidget.fillInfo (filename, tuple, decoder, updating_enabled);
}

void InfoWindow::displayImage (const char * filename)
{
    if (! strcmp_safe (filename, m_filename))
        m_image.setPixmap (art_request (filename, 2 * sizes.OneInch, 2 * sizes.OneInch));
}

static InfoWindow * s_infowin = nullptr;

static void show_infowin (const char * filename,
 const Tuple & tuple, PluginHandle * decoder, bool can_write)
{
    if (! s_infowin)
    {
        s_infowin = new InfoWindow;
        s_infowin->setAttribute (Qt::WA_DeleteOnClose);

        QObject::connect (s_infowin, & QObject::destroyed, [] () {
            s_infowin = nullptr;
        });
    }

    s_infowin->fillInfo (filename, tuple, decoder, can_write);
    s_infowin->resize (6 * sizes.OneInch, 3 * sizes.OneInch);
    window_bring_to_front (s_infowin);
}

EXPORT void infowin_show (Playlist playlist, int entry)
{
    String filename = playlist.entry_filename (entry);
    if (! filename)
        return;

    String error;
    PluginHandle * decoder = playlist.entry_decoder (entry, Playlist::Wait, & error);
    Tuple tuple = decoder ? playlist.entry_tuple (entry, Playlist::Wait, & error) : Tuple ();

    if (decoder && tuple.valid () && ! aud_custom_infowin (filename, decoder))
    {
        /* cuesheet entries cannot be updated */
        bool can_write = aud_file_can_write_tuple (filename, decoder) &&
         ! tuple.is_set (Tuple::StartTime);

        tuple.delete_fallbacks ();
        show_infowin (filename, tuple, decoder, can_write);
    }
    else
        infowin_hide ();

    if (error)
        aud_ui_show_error (str_printf (_("Error opening %s:\n%s"),
         (const char *) filename, (const char *) error));
}

EXPORT void infowin_show_current ()
{
    auto playlist = Playlist::playing_playlist ();
    if (playlist == Playlist ())
        playlist = Playlist::active_playlist ();

    int position = playlist.get_position ();
    if (position < 0)
        return;

    infowin_show (playlist, position);
}

EXPORT void infowin_hide ()
{
    delete s_infowin;
}

} // namespace audqt
