/**
 * \file
 *
 * \author Mattia Basaglia
 *
 * \copyright Copyright (C) 2015 Mattia Basaglia
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "main_window.hpp"
#include "main_window_p.hpp"

#include <QImageReader>
#include <QImageWriter>
#include <QFileDialog>
#include <QMessageBox>

#include "cayman/data.hpp"
#include "confirm_close_dialog.hpp"
#include "cayman/message.hpp"
#include "tool/registry.hpp"
#include "dialog_document_create.hpp"

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), p(new Private(this))
{
    p->setupUi(this);

    setWindowIcon(cayman::data().caymanIcon("pixel-cayman-eye", 24));

    p->initDocks();
    p->initMenus();
    p->initStatusBar();
    p->loadSettings();
    p->setCurrentView(nullptr);

    connect(p->main_tab, &QTabWidget::tabCloseRequested,
            this, &MainWindow::closeTabPrompt);

    connect(p->main_tab, &QTabWidget::currentChanged, [this](int tab) {
        p->setCurrentTab(tab);
    });

    p->current_color_selector.color->setColor(Qt::black);

    cayman::Message::manager().setDialogParent(this);


    /// \todo Dynamic registration/unregistration facilities
    for ( auto tool : ::tool::Registry::instance().tools() )
        addTool(tool);
}

MainWindow::~MainWindow()
{
    p->saveSettings();
    p->dock_tool_options->setWidget(nullptr);
    cayman::Message::manager().setDialogParent(nullptr);
    disconnect(p->log_view_connection);
    delete p;
}

void MainWindow::changeEvent(QEvent* event)
{
    if ( event->type() == QEvent::LanguageChange )
    {
        p->retranslateUi(this);
        p->current_color_selector.retranslateUi(p->dock_current_color->widget());
        p->translateDocks();
        p->translateMenus();
        p->translateStatusBar();
    }

    QMainWindow::changeEvent(event);
}

void MainWindow::setActiveColor(const QColor& color)
{
    p->color_editor->setColor(color);
}

bool MainWindow::documentNew()
{
    /// \todo Show dialog to get the size
    DialogDocumentCreate dlg(this);
    if ( p->current_view )
        dlg.setImageSize(p->current_view->document()->imageSize());
    if ( !dlg.exec() )
        return false;

    document::Document* doc = new document::Document(dlg.imageSize(), {}, dlg.background());
    p->addDocument(doc, true);
    return true;
}

bool MainWindow::documentOpen()
{
    QString default_dir;
    if ( p->current_view )
    {
        if ( !p->current_view->document()->fileName().isEmpty() )
            default_dir = QFileInfo(p->current_view->document()->fileName()).dir().path();
    }

    auto action = io::Formats::Action::Open;
    auto filters = io::formats().nameFilters(action);
    QFileDialog open_dialog(this, tr("Open Image"), default_dir);
    open_dialog.setFileMode(QFileDialog::ExistingFiles);
    open_dialog.setAcceptMode(QFileDialog::AcceptOpen);
    open_dialog.setNameFilters(filters);
    open_dialog.selectNameFilter(filters[filters.size()-2]);

    if ( !open_dialog.exec() )
        return false;

    io::AbstractFormat* format
        = io::formats().formatFromNameFilter(open_dialog.selectedNameFilter(), action);

    int tab = -1;
    for ( const QString& file_name : open_dialog.selectedFiles() )
    {
        int new_tab = openTab(file_name, false, format);
        if ( new_tab != -1 )
            tab = new_tab;
    }

    if ( tab != -1 )
    {
        p->main_tab->setCurrentIndex(tab);
        return true;
    }

    return false;
}

bool MainWindow::documentSave()
{
    return save(p->main_tab->currentIndex(), false);
}

bool MainWindow::documentSaveAs()
{
    return save(p->main_tab->currentIndex(), true);
}

bool MainWindow::save(int tab, bool prompt)
{
    view::GraphicsWidget* widget = p->widget(tab);
    if ( !widget )
        return false;

    document::Document* doc = widget->document();

    if ( !doc )
        return false;

    if ( doc->fileName().isEmpty() )
        prompt = true;

    auto action = io::Formats::Action::Save;

    io::AbstractFormat* format = doc->formatSettings().preferred();
    if ( !format )
        format = io::formats().formatFromFileName(doc->fileName(), action);

    if ( prompt || !format )
    {
        // Ensure the the image is visible so the user knows what they are saving
        if ( tab != p->main_tab->currentIndex() )
            p->main_tab->setCurrentIndex(tab);

        QFileDialog save_dialog(this, tr("Save Image"), doc->fileName());
        save_dialog.setFileMode(QFileDialog::AnyFile);
        save_dialog.setAcceptMode(QFileDialog::AcceptSave);
        auto filters = io::formats().nameFilters(action);
        save_dialog.setNameFilters(filters);
        if ( format )
            save_dialog.selectNameFilter(format->nameFilter(action));
        else
            save_dialog.selectNameFilter(filters[filters.size()-2]);


        if ( !save_dialog.exec() )
            return false;

        /// \todo Ask confirmation if the file exists
        /// QFileDialog should already do this but it doesnt,
        /// At least not with the native KDE dialog.

        QString selected_file = save_dialog.selectedFiles().front();

        format = io::formats().formatFromNameFilter(save_dialog.selectedNameFilter(), action);
        if ( !format )
        {
            format = io::formats().formatFromFileName(selected_file, action);
            if ( !format )
            {
                cayman::Message(Msg::Dialog|Msg::Error) << tr("Unknown format");
                return false;
            }
        }
        else if ( QFileInfo(selected_file).suffix().isEmpty() )
        {
            selected_file.append("."+
                format->extensions(io::AbstractFormat::Action::Save).front());
        }

        doc->formatSettings().setPreferred(format);
        doc->setFileName(selected_file);
        p->main_tab->setTabText(tab, p->documentName(doc));
    }

    if ( format->save(doc) )
    {
        doc->undoStack().setClean();
        if ( format->canOpen() )
            p->pushRecentFile(doc->fileName());
        p->updateTitle();
    }
    else
    {
        cayman::Message(Msg::AllOutput|Msg::Error)
            << tr("Error saving %1: %2").arg(doc->fileName()).arg(format->errorString());
    }
    return false;
}

int MainWindow::openTab(const QString& file_name, bool set_current,
                        io::AbstractFormat* format)
{
    if ( !format )
    {
        format = io::formats().formatFromFileName(file_name,io::Formats::Action::Open);
        if ( !format )
        {
            cayman::Message(Msg::Dialog|Msg::Error)
                << tr("Unknown format for %1").arg(file_name);
            return -1;
        }
    }

    document::Document* doc = format->open(file_name);
    if ( doc )
    {
        doc->formatSettings().setPreferred(format);
        p->pushRecentFile(doc->fileName());
        return p->addDocument(doc, set_current);
    }

    cayman::Message(Msg::AllOutput|Msg::Error)
        << tr("Error opening %1: %2").arg(file_name).arg(format->errorString());

    return -1;
}

bool MainWindow::documentClose()
{
    return closeTabPrompt(p->main_tab->currentIndex());
}

bool MainWindow::closeTab(int tab, bool prompt)
{
    view::GraphicsWidget* widget = p->widget(tab);

    if ( !widget )
        return false;

    if ( prompt && !widget->document()->undoStack().isClean() )
    {
         int reply = QMessageBox::question(this,
            tr("Close file"),
            tr("\"%1\" has been modified.\nDo you want to save the changes?")
                .arg(p->documentName(widget->document())),
            QMessageBox::Yes|QMessageBox::No|QMessageBox::Cancel,
            QMessageBox::Yes
        );
        if ( reply == QMessageBox::Yes )
            save(tab, false);
        else if ( reply == QMessageBox::Cancel )
            return false;
    }

    if ( widget == p->current_view )
    {
        p->setCurrentView(nullptr);
    }
    
    p->undo_group.removeStack(&widget->document()->undoStack());
    delete widget->document();
    delete widget;

    return true;
}

bool MainWindow::closeTabPrompt(int tab)
{
    return closeTab(tab, p->confirm_close);
}

void MainWindow::addTool(::tool::Tool* tool)
{
    if ( !tool || p->tools.contains(tool) )
        return;

    p->tools.push_back(tool);
    /// \todo Retranslate them(?)
    QAction* tool_action = new QAction(tool->icon(), tool->name(), p->tools_group);
    tool_action->setCheckable(true);
    p->menu_tools->addAction(tool_action);
    p->tool_bar->addAction(tool_action);
    connect(tool_action, &QAction::triggered, [this, tool](bool checked){
        ::tool::Tool* used_tool = checked ? tool : nullptr;
        p->current_tool = used_tool;
        if ( p->current_view )
            p->current_view->setCurrentTool(used_tool);
        p->dock_tool_options->setWidget(used_tool ? used_tool->optionsWidget() : nullptr);
    });
}

bool MainWindow::documentCloseAll()
{
    if ( p->main_tab->count() != 0 )
    {
        ConfirmCloseDialog dlg(this);

        bool has_dirty_documents = false;
        if ( p->confirm_close )
        {
            for ( int i = 0; i < p->main_tab->count(); i++ )
            {
                auto widget = p->widget(i);
                if ( !widget->document()->undoStack().isClean() )
                {
                    dlg.addFile(i, p->documentName(widget->document()));
                    has_dirty_documents = true;
                }
            }
        }

        if ( has_dirty_documents )
        {
            if ( !dlg.exec() )
                return false;

            /// \todo should return false here only if the user has canceled the save
            for ( int i : dlg.saveFiles() )
                if ( !save(i, false) )
                    return false;
        }
        
        while ( p->main_tab->count() != 0 )
            closeTab(0, false);
    }

    return true;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if ( !documentCloseAll() )
    {
        event->ignore();
        return;
    }

    QMainWindow::closeEvent(event);
}

bool MainWindow::documentReload()
{
    if ( !p->current_view )
        return false;

    auto document = p->current_view->document();

    auto filename = document->fileName();
    if ( filename.isEmpty() )
        return false;

    auto format = document->formatSettings().preferred();
    if ( !format )
    {
        format = io::formats().formatFromFileName(filename, io::Formats::Action::Open);
    }


    if ( !format || !format->canOpen() )
    {
        cayman::Message(Msg::AllOutput|Msg::Error,
            tr("Error opening %1: %2")
                .arg(filename).arg(tr("Format not supported")));
        return false;
    }

    auto new_doc = format->open(filename);
    if ( !new_doc )
    {
        cayman::Message(Msg::AllOutput|Msg::Error,
            tr("Error opening %1: %2")
                .arg(filename).arg(format->errorString()));
        return false;
    }

    ::document::visitor::Move move(document, tr("Reload from file"));
    new_doc->apply(move);
    delete new_doc;
    document->undoStack().setClean();

    return true;
}

void MainWindow::clearSettings()
{
    p->clearSettings();
}
