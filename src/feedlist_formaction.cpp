#include <feedlist_formaction.h>
#include <view.h>
#include <config.h>
#include <cassert>
#include <logger.h>
#include <reloadthread.h>
#include <exceptions.h>
#include <utils.h>
#include <formatstring.h>

#include <listformatter.h>

#include <sstream>
#include <string>

#include <langinfo.h>

#define FILTER_UNREAD_FEEDS "unread_count != \"0\""

namespace newsbeuter {

feedlist_formaction::feedlist_formaction(view * vv, std::string formstr) 
	: formaction(vv,formstr), zero_feedpos(false), feeds_shown(0),
		auto_open(false), quit(false), apply_filter(false), search_dummy_feed(new rss_feed(v->get_ctrl()->get_cache())),
		filterpos(0), set_filterpos(false), rxman(0), old_width(0) {
	assert(true==m.parse(FILTER_UNREAD_FEEDS));
}

void feedlist_formaction::init() {
	set_keymap_hints();

	f->run(-3); // compute all widget dimensions

	if(v->get_ctrl()->get_refresh_on_start()) {
		f->run(-1); // FRUN
		v->get_ctrl()->start_reload_all_thread();
	}
	v->get_ctrl()->update_feedlist();

	/*
	 * This is kind of a hack.
	 * The feedlist_formaction is responsible for starting up the reloadthread, which is responsible
	 * for regularly spawning downloadthreads.
	 */
	time_t reload_cycle = 60 * v->get_cfg()->get_configvalue_as_int("reload-time");
	if (v->get_cfg()->get_configvalue_as_bool("auto-reload") == true) {
		f->run(-1); // FRUN
		reloadthread  * rt = new reloadthread(v->get_ctrl(), reload_cycle, v->get_cfg());
		rt->start();
	}

	apply_filter = !(v->get_cfg()->get_configvalue_as_bool("show-read-feeds"));
}

feedlist_formaction::~feedlist_formaction() { }

void feedlist_formaction::prepare() {
	unsigned int width = utils::to_u(f->get("items:w"));

	if (old_width != width) {
		do_redraw = true;
		old_width = width;
		GetLogger().log(LOG_DEBUG, "feedlist_formaction::prepare: apparent resize");
	}

	if (do_redraw) {
		GetLogger().log(LOG_DEBUG, "feedlist_formaction::prepare: doing redraw");
		do_redraw = false;
		v->get_ctrl()->update_feedlist();
		set_pos();
	}
}

void feedlist_formaction::process_operation(operation op, bool automatic, std::vector<std::string> * args) {
	std::string feedpos = f->get("feedposname");
	std::istringstream posname(feedpos);
	unsigned int pos = 0;
	posname >> pos;
	switch (op) {
		case OP_OPEN: {
				if (f->get_focus() == "feeds") {
					if (automatic && args->size() > 0) {
						std::istringstream x((*args)[0]);
						x >> pos;
					}
					GetLogger().log(LOG_INFO, "feedlist_formaction: opening feed at position `%s'",feedpos.c_str());
					if (feeds_shown > 0 && feedpos.length() > 0) {
						v->push_itemlist(pos);
					} else {
						v->show_error(_("No feed selected!")); // should not happen
					}
				}
			}
			break;
		case OP_RELOAD: {
				GetLogger().log(LOG_INFO, "feedlist_formaction: reloading feed at position `%s'",feedpos.c_str());
				if (feeds_shown > 0 && feedpos.length() > 0) {
					v->get_ctrl()->reload(pos);
				} else {
					v->show_error(_("No feed selected!")); // should not happen
				}
			}
			break;
		case OP_INT_RESIZE:
			do_redraw = true;
			break;
		case OP_RELOADURLS:
			v->get_ctrl()->reload_urls_file();
			break;
		case OP_OPENINBROWSER: {
				std::tr1::shared_ptr<rss_feed> feed = v->get_ctrl()->get_feed(pos);
				if (feed) {
					v->open_in_browser(feed->link());
				}
			}
			break;
		case OP_RELOADALL:
			GetLogger().log(LOG_INFO, "feedlist_formaction: reloading all feeds");
			{
				bool reload_only_visible_feeds = v->get_cfg()->get_configvalue_as_bool("reload-only-visible-feeds");
				std::vector<int> idxs;
				for (std::vector<feedptr_pos_pair>::iterator it=visible_feeds.begin();it!=visible_feeds.end();++it) {
					idxs.push_back(it->second);
				}
				v->get_ctrl()->start_reload_all_thread(reload_only_visible_feeds ? &idxs : NULL);
			}
			break;
		case OP_MARKFEEDREAD: {
				GetLogger().log(LOG_INFO, "feedlist_formaction: marking feed read at position `%s'",feedpos.c_str());
				if (feeds_shown > 0 && feedpos.length() > 0) {
					v->set_status(_("Marking feed read..."));
					try {
						v->get_ctrl()->mark_all_read(pos);
						do_redraw = true;
						v->set_status("");
					} catch (const dbexception& e) {
						v->show_error(utils::strprintf(_("Error: couldn't mark feed read: %s"), e.what()));
					}
				} else {
					v->show_error(_("No feed selected!")); // should not happen
				}
			}
			break;
		case OP_TOGGLESHOWREAD:
			m.parse(FILTER_UNREAD_FEEDS);
			GetLogger().log(LOG_INFO, "feedlist_formaction: toggling show-read-feeds");
			if (v->get_cfg()->get_configvalue_as_bool("show-read-feeds")) {
				v->get_cfg()->set_configvalue("show-read-feeds","no");
				apply_filter = true;
			} else {
				v->get_cfg()->set_configvalue("show-read-feeds","yes");
				apply_filter = false;
			}
			save_filterpos();
			do_redraw = true;
			break;
		case OP_NEXTUNREAD: {
				unsigned int local_tmp;
				GetLogger().log(LOG_INFO, "feedlist_formaction: jumping to next unred feed");
				if (!jump_to_next_unread_feed(local_tmp)) {
					v->show_error(_("No feeds with unread items."));
				}
			}
			break;
		case OP_PREVUNREAD: {
				unsigned int local_tmp;
				GetLogger().log(LOG_INFO, "feedlist_formaction: jumping to previous unred feed");
				if (!jump_to_previous_unread_feed(local_tmp)) {
					v->show_error(_("No feeds with unread items."));
				}
			}
			break;
		case OP_MARKALLFEEDSREAD:
			GetLogger().log(LOG_INFO, "feedlist_formaction: marking all feeds read");
			v->set_status(_("Marking all feeds read..."));
			v->get_ctrl()->catchup_all();
			v->set_status("");
			do_redraw = true;
			break;
		case OP_CLEARTAG:
			tag = "";
			do_redraw = true;
			zero_feedpos = true;
			break;
		case OP_SETTAG: 
			if (tags.size() > 0) {
				std::string newtag;
				if (automatic && args->size() > 0) {
					newtag = (*args)[0];
				} else {
					newtag = v->select_tag(tags);
				}
				if (newtag != "") {
					tag = newtag;
					do_redraw = true;
					zero_feedpos = true;
				}
			} else {
				v->show_error(_("No tags defined."));
			}
			break;
		case OP_SELECTFILTER:
			if (v->get_ctrl()->get_filters().size() > 0) {
				std::string newfilter;
				if (automatic && args->size() > 0) {
					newfilter = (*args)[0];
				} else {
					newfilter = v->select_filter(v->get_ctrl()->get_filters().get_filters());
				}
				if (newfilter != "") {
					filterhistory.add_line(newfilter);
					if (newfilter.length() > 0) {
						if (!m.parse(newfilter)) {
							v->show_error(_("Error: couldn't parse filter command!"));
							m.parse(FILTER_UNREAD_FEEDS);
						} else {
							save_filterpos();
							apply_filter = true;
							do_redraw = true;
						}
					}
				}
			} else {
				v->show_error(_("No filters defined."));
			}
			break;
		case OP_SEARCH:
			if (automatic && args->size() > 0) {
				qna_responses.clear();
				// when in automatic mode, we manually fill the qna_responses vector from the arguments
				// and then run the finished_qna() by ourselves to simulate a "Q&A" session that is
				// in fact macro-driven.
				qna_responses.push_back((*args)[0]);
				finished_qna(OP_INT_START_SEARCH);
			} else {
				std::vector<qna_pair> qna;
				qna.push_back(qna_pair(_("Search for: "), ""));
				this->start_qna(qna, OP_INT_START_SEARCH, &searchhistory);
			}
			break;
		case OP_CLEARFILTER:
			apply_filter = !(v->get_cfg()->get_configvalue_as_bool("show-read-feeds"));
			m.parse(FILTER_UNREAD_FEEDS);
			do_redraw = true;
			save_filterpos();
			break;
		case OP_SETFILTER:
			if (automatic && args->size() > 0) {
				qna_responses.clear();
				qna_responses.push_back((*args)[0]);
				finished_qna(OP_INT_END_SETFILTER);
			} else {
				std::vector<qna_pair> qna;
				qna.push_back(qna_pair(_("Filter: "), ""));
				this->start_qna(qna, OP_INT_END_SETFILTER, &filterhistory);
			}
			break;
		case OP_EDIT_URLS:
			v->get_ctrl()->edit_urls_file();
			break;
		case OP_QUIT:
			GetLogger().log(LOG_INFO, "feedlist_formaction: quitting");
			if (automatic || !v->get_cfg()->get_configvalue_as_bool("confirm-exit") || v->confirm(_("Do you really want to quit (y:Yes n:No)? "), _("yn")) == *_("y")) {
				quit = true;
			}
			break;
		case OP_HELP:
			v->push_help();
			break;
		default:
			break;
	}
	if (quit) {
		v->pop_current_formaction();
	}
}

void feedlist_formaction::update_visible_feeds(std::vector<std::tr1::shared_ptr<rss_feed> >& feeds) {
	assert(v->get_cfg() != NULL); // must not happen

	visible_feeds.clear();

	unsigned int i = 0;

	for (std::vector<std::tr1::shared_ptr<rss_feed> >::iterator it = feeds.begin(); it != feeds.end(); ++it, ++i) {
		if ((tag == "" || (*it)->matches_tag(tag)) && (!apply_filter || m.matches(it->get()))) {
			visible_feeds.push_back(feedptr_pos_pair(*it,i));
		}
	}

	feeds_shown = visible_feeds.size();
}

void feedlist_formaction::set_feedlist(std::vector<std::tr1::shared_ptr<rss_feed> >& feeds) {
	assert(v->get_cfg() != NULL); // must not happen

	unsigned int width = utils::to_u(f->get("feeds:w"));

	unsigned int i = 0;
	unsigned int unread_feeds = 0;

	std::string feedlist_format = v->get_cfg()->get_configvalue("feedlist-format");

	listformatter listfmt;

	update_visible_feeds(feeds);

	for (std::vector<feedptr_pos_pair>::iterator it = visible_feeds.begin(); it != visible_feeds.end(); ++it, ++i) {
		std::string title = get_title(it->first);

		if (it->first->unread_item_count() > 0)
			++unread_feeds;

		listfmt.add_line(format_line(feedlist_format, it->first, it->second, width), it->second);
	}

	f->modify("feeds","replace_inner",listfmt.format_list(rxman, "feedlist"));

	std::string title_format = v->get_cfg()->get_configvalue("feedlist-title-format");

	fmtstr_formatter fmt;
	fmt.register_fmt('T', tag);
	fmt.register_fmt('N', PROGRAM_NAME);
	fmt.register_fmt('V', PROGRAM_VERSION);
	fmt.register_fmt('u', utils::to_s(unread_feeds));
	fmt.register_fmt('t', utils::to_s(i));

	f->set("head", fmt.do_format(title_format, width));
}

void feedlist_formaction::set_tags(const std::vector<std::string>& t) {
	tags = t;
}

keymap_hint_entry * feedlist_formaction::get_keymap_hint() {
	static keymap_hint_entry hints[] = {
		{ OP_QUIT, _("Quit") },
		{ OP_OPEN, _("Open") },
		{ OP_NEXTUNREAD, _("Next Unread") },
		{ OP_RELOAD, _("Reload") },
		{ OP_RELOADALL, _("Reload All") },
		{ OP_MARKFEEDREAD, _("Mark Read") },
		{ OP_MARKALLFEEDSREAD, _("Catchup All") },
		{ OP_SEARCH, _("Search") },
		{ OP_HELP, _("Help") },
		{ OP_NIL, NULL }
	};
	return hints;
}

bool feedlist_formaction::jump_to_previous_unread_feed(unsigned int& feedpos) {
	unsigned int curpos;
	std::istringstream is(f->get("feedpos"));
	is >> curpos;
	GetLogger().log(LOG_DEBUG, "feedlist_formaction::jump_to_previous_unread_feed: searching for unread feed");

	for (int i=curpos-1;i>=0;--i) {
		GetLogger().log(LOG_DEBUG, "feedlist_formaction::jump_to_previous_unread_feed: visible_feeds[%u] unread items: %u", i, visible_feeds[i].first->unread_item_count());
		if (visible_feeds[i].first->unread_item_count() > 0) {
			GetLogger().log(LOG_DEBUG, "feedlist_formaction::jump_to_previous_unread_feed: hit");
			f->set("feedpos", utils::to_s(i));
			feedpos = visible_feeds[i].second;
			return true;
		}
	}
	for (int i=visible_feeds.size()-1;i>=static_cast<int>(curpos);--i) {
		GetLogger().log(LOG_DEBUG, "feedlist_formaction::jump_to_previous_unread_feed: visible_feeds[%u] unread items: %u", i, visible_feeds[i].first->unread_item_count());
		if (visible_feeds[i].first->unread_item_count() > 0) {
			GetLogger().log(LOG_DEBUG, "feedlist_formaction::jump_to_previous_unread_feed: hit");
			f->set("feedpos", utils::to_s(i));
			feedpos = visible_feeds[i].second;
			return true;
		}
	}
	return false;
}

void feedlist_formaction::goto_feed(const std::string& str) {
	unsigned int curpos;
	std::istringstream is(f->get("feedpos"));
	is >> curpos;
	GetLogger().log(LOG_DEBUG, "feedlist_formaction::goto_feed: curpos = %u str = `%s'", curpos, str.c_str());
	for (unsigned int i=curpos+1;i<visible_feeds.size();++i) {
		if (strcasestr(visible_feeds[i].first->title().c_str(), str.c_str()) != NULL) {
			f->set("feedpos", utils::to_s(i));
			return;
		}
	}
	for (unsigned int i=0;i<=curpos;++i) {
		if (strcasestr(visible_feeds[i].first->title().c_str(), str.c_str()) != NULL) {
			f->set("feedpos", utils::to_s(i));
			return;
		}
	}
}

bool feedlist_formaction::jump_to_next_unread_feed(unsigned int& feedpos) {
	unsigned int curpos;
	std::istringstream is(f->get("feedpos"));
	is >> curpos;
	GetLogger().log(LOG_DEBUG, "feedlist_formaction::jump_to_next_unread_feed: searching for unread feed");

	for (unsigned int i=curpos+1;i<visible_feeds.size();++i) {
		GetLogger().log(LOG_DEBUG, "feedlist_formaction::jump_to_next_unread_feed: visible_feeds[%u] unread items: %u", i, visible_feeds[i].first->unread_item_count());
		if (visible_feeds[i].first->unread_item_count() > 0) {
			GetLogger().log(LOG_DEBUG, "feedlist_formaction::jump_to_next_unread_feed: hit");
			f->set("feedpos", utils::to_s(i));
			feedpos = visible_feeds[i].second;
			return true;
		}
	}
	for (unsigned int i=0;i<=curpos;++i) {
		GetLogger().log(LOG_DEBUG, "feedlist_formaction::jump_to_next_unread_feed: visible_feeds[%u] unread items: %u", i, visible_feeds[i].first->unread_item_count());
		if (visible_feeds[i].first->unread_item_count() > 0) {
			GetLogger().log(LOG_DEBUG, "feedlist_formaction::jump_to_next_unread_feed: hit");
			f->set("feedpos", utils::to_s(i));
			feedpos = visible_feeds[i].second;
			return true;
		}
	}
	return false;
}

std::tr1::shared_ptr<rss_feed> feedlist_formaction::get_feed() {
	unsigned int curpos;
	std::istringstream is(f->get("feedpos"));
	is >> curpos;
	return visible_feeds[curpos].first;
}

int feedlist_formaction::get_pos(unsigned int realidx) {
	for (unsigned int i=0;i<visible_feeds.size();++i) {
		if (visible_feeds[i].second == realidx)
			return i;
	}
	return -1;
}

void feedlist_formaction::handle_cmdline(const std::string& cmd) {
	unsigned int idx = 0;
	/*
	 * this handle_cmdline is a bit different than the other ones.
	 * Since we want to use ":30" to jump to the 30th entry, we first
	 * need to check whether the command parses as unsigned integer,
	 * and if so, jump to the entered entry. Otherwise, we try to
	 * handle it as a normal command.
	 */
	if (1==sscanf(cmd.c_str(),"%u",&idx)) {
		handle_cmdline_num(idx);
	} else {
		// hand over all other commands to formaction
		std::vector<std::string> tokens = utils::tokenize_quoted(cmd, " \t");
		if (tokens.size() > 0) {
			if (tokens[0] == "tag") {
				if (tokens.size() >= 2 && tokens[1] != "") {
					tag = tokens[1];
					do_redraw = true;
					zero_feedpos = true;
				}
			} else if (tokens[0] == "goto") {
				if (tokens.size() >= 2 && tokens[1] != "") {
					goto_feed(tokens[1]);
				}
			} else {
				formaction::handle_cmdline(cmd);
			}
		}
	}
}

void feedlist_formaction::finished_qna(operation op) {
	formaction::finished_qna(op); // important!

	switch (op) {
		case OP_INT_END_SETFILTER:
			op_end_setfilter();
			break;
		case OP_INT_START_SEARCH:
			op_start_search();
			break;
		default:
			break;
	}
}

void feedlist_formaction::mark_pos_if_visible(unsigned int pos) {
	scope_measure m1("feedlist_formaction::mark_pos_if_visible");
	unsigned int vpos = 0;
	v->get_ctrl()->update_visible_feeds();
	for (std::vector<feedptr_pos_pair>::iterator it=visible_feeds.begin();it!=visible_feeds.end();++it, ++vpos) {
		if (it->second == pos) {
			GetLogger().log(LOG_DEBUG, "feedlist_formaction::mark_pos_if_visible: match, setting position to %u", vpos);
			f->set("feedpos", utils::to_s(vpos));
			break;
		}
	}
}

void feedlist_formaction::save_filterpos() {
	std::istringstream is(f->get("feedpos"));
	unsigned int i;
	is >> i;
	if (i<visible_feeds.size()) {
		filterpos = visible_feeds[i].second;
		set_filterpos = true;
	}
}

void feedlist_formaction::set_regexmanager(regexmanager * r) {
	rxman = r;
	std::vector<std::string>& attrs = r->get_attrs("feedlist");
	unsigned int i=0;
	std::string attrstr;
	for (std::vector<std::string>::iterator it=attrs.begin();it!=attrs.end();++it,++i) {
		attrstr.append(utils::strprintf("@style_%u_normal:%s ", i, it->c_str()));
		attrstr.append(utils::strprintf("@style_%u_focus:%s ", i, it->c_str()));
	}
	std::string textview = utils::strprintf("{!list[feeds] .expand:vh style_normal[listnormal]: style_focus[listfocus]:fg=yellow,bg=blue,attr=bold pos_name[feedposname]: pos[feedpos]:0 %s richtext:1}", attrstr.c_str());
	f->modify("feeds", "replace", textview);
}


void feedlist_formaction::op_end_setfilter() {
	std::string filtertext = qna_responses[0];
	filterhistory.add_line(filtertext);
	if (filtertext.length() > 0) {
		if (!m.parse(filtertext)) {
			v->show_error(_("Error: couldn't parse filter command!"));
			m.parse(FILTER_UNREAD_FEEDS);
		} else {
			save_filterpos();
			apply_filter = true;
			do_redraw = true;
		}
	}
}


void feedlist_formaction::op_start_search() {
	std::string searchphrase = qna_responses[0];
	GetLogger().log(LOG_DEBUG, "feedlist_formaction::op_start_search: starting search for `%s'", searchphrase.c_str());
	if (searchphrase.length() > 0) {
		v->set_status(_("Searching..."));
		searchhistory.add_line(searchphrase);
		std::vector<std::tr1::shared_ptr<rss_item> > items;
		try {
			std::string utf8searchphrase = utils::convert_text(searchphrase, "utf-8", nl_langinfo(CODESET));
			items = v->get_ctrl()->search_for_items(utf8searchphrase, "");
		} catch (const dbexception& e) {
			v->show_error(utils::strprintf(_("Error while searching for `%s': %s"), searchphrase.c_str(), e.what()));
			return;
		}
		if (items.size() > 0) {
			for (std::vector<std::tr1::shared_ptr<rss_item> >::iterator it=items.begin();it!=items.end();++it) {
				(*it)->set_feedptr(search_dummy_feed);
			}
			search_dummy_feed->items() = items;
			v->push_searchresult(search_dummy_feed);
		} else {
			v->show_error(_("No results."));
		}
	}
}

void feedlist_formaction::handle_cmdline_num(unsigned int idx) {
	if (idx > 0 && idx <= (visible_feeds[visible_feeds.size()-1].second + 1)) {
		int i = get_pos(idx - 1);
		if (i == -1) {
			v->show_error(_("Position not visible!"));
		} else {
			f->set("feedpos", utils::to_s(i));
		}
	} else {
		v->show_error(_("Invalid position!"));
	}
}

void feedlist_formaction::set_pos() {
	if (set_filterpos) {
		set_filterpos = false;
		unsigned int i = 0;
		for (std::vector<feedptr_pos_pair>::iterator it=visible_feeds.begin();it!=visible_feeds.end();++it, ++i) {
			if (it->second == filterpos) {
				f->set("feedpos", utils::to_s(i));
				return;
			}
		}
		f->set("feedpos", "0");
	} else if (zero_feedpos) {
		f->set("feedpos","0");
		zero_feedpos = false;
	}
}

std::string feedlist_formaction::get_title(std::tr1::shared_ptr<rss_feed> feed) {
	std::string title = feed->title();
	if (title.length()==0)
		title = feed->rssurl();
	if (title.length()==0)
		title = "<no title>";
	return title;
}

std::string feedlist_formaction::format_line(const std::string& feedlist_format, std::tr1::shared_ptr<rss_feed> feed, unsigned int pos, unsigned int width) {
	fmtstr_formatter fmt;
	unsigned int unread_count = feed->unread_item_count();

	fmt.register_fmt('i', utils::strprintf("%u", pos + 1));
	fmt.register_fmt('u', utils::strprintf("(%u/%u)",unread_count,static_cast<unsigned int>(feed->items().size())));
	fmt.register_fmt('n', unread_count > 0 ? "N" : " ");
	fmt.register_fmt('t', get_title(feed));
	fmt.register_fmt('T', feed->get_firsttag());
	fmt.register_fmt('l', feed->link());
	fmt.register_fmt('L', feed->rssurl());
	fmt.register_fmt('d', feed->description());

	std::string format = fmt.do_format(feedlist_format, width);
	GetLogger().log(LOG_DEBUG, "feedlist_formaction::set_feedlist: format result = %s", format.c_str());

	return format;
}

}
