/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2014  Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "otpch.h"

#include "iomarket.h"

#include "configmanager.h"
#include "databasetasks.h"
#include "iologindata.h"
#include "game.h"
#include "scheduler.h"

extern ConfigManager g_config;
extern Game g_game;

MarketOfferList IOMarket::getActiveOffers(MarketAction_t action, uint16_t itemId)
{
	MarketOfferList offerList;

	std::ostringstream query;
	query << "SELECT `id`, `amount`, `price`, `created`, `anonymous`, (SELECT `name` FROM `players` WHERE `id` = `player_id`) AS `player_name` FROM `market_offers` WHERE `sale` = " << action << " AND `itemtype` = " << itemId;

	DBResult_ptr result = Database::getInstance()->storeQuery(query.str());
	if (!result) {
		return offerList;
	}

	const int32_t marketOfferDuration = g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);

	do {
		MarketOffer offer;
		offer.amount = result->getDataInt("amount");
		offer.price = result->getDataInt("price");
		offer.timestamp = result->getDataInt("created") + marketOfferDuration;
		offer.counter = result->getDataInt("id") & 0xFFFF;
		if (result->getDataInt("anonymous") == 0) {
			offer.playerName = result->getDataString("player_name");
		} else {
			offer.playerName = "Anonymous";
		}
		offerList.push_back(offer);
	} while (result->next());
	return offerList;
}

MarketOfferList IOMarket::getOwnOffers(MarketAction_t action, uint32_t playerId)
{
	MarketOfferList offerList;

	const int32_t marketOfferDuration = g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);

	std::ostringstream query;
	query << "SELECT `id`, `amount`, `price`, `created`, `itemtype` FROM `market_offers` WHERE `player_id` = " << playerId << " AND `sale` = " << action;

	DBResult_ptr result = Database::getInstance()->storeQuery(query.str());
	if (!result) {
		return offerList;
	}

	do {
		MarketOffer offer;
		offer.amount = result->getDataInt("amount");
		offer.price = result->getDataInt("price");
		offer.timestamp = result->getDataInt("created") + marketOfferDuration;
		offer.counter = result->getDataInt("id") & 0xFFFF;
		offer.itemId = result->getDataInt("itemtype");
		offerList.push_back(offer);
	} while (result->next());
	return offerList;
}

HistoryMarketOfferList IOMarket::getOwnHistory(MarketAction_t action, uint32_t playerId)
{
	HistoryMarketOfferList offerList;

	std::ostringstream query;
	query << "SELECT `itemtype`, `amount`, `price`, `expires_at`, `state` FROM `market_history` WHERE `player_id` = " << playerId << " AND `sale` = " << action;

	DBResult_ptr result = Database::getInstance()->storeQuery(query.str());
	if (!result) {
		return offerList;
	}

	do {
		HistoryMarketOffer offer;
		offer.itemId = result->getDataInt("itemtype");
		offer.amount = result->getDataInt("amount");
		offer.price = result->getDataInt("price");
		offer.timestamp = result->getDataInt("expires_at");

		MarketOfferState_t offerState = static_cast<MarketOfferState_t>(result->getDataInt("state"));
		if (offerState == OFFERSTATE_ACCEPTEDEX) {
			offerState = OFFERSTATE_ACCEPTED;
		}

		offer.state = offerState;

		offerList.push_back(offer);
	} while (result->next());
	return offerList;
}

void IOMarket::processExpiredOffers(DBResult_ptr result, bool)
{
	if (!result) {
		return;
	}

	do {
		if (!IOMarket::moveOfferToHistory(result->getNumber<uint32_t>("id"), OFFERSTATE_EXPIRED)) {
			continue;
		}

		const uint32_t playerId = result->getNumber<uint32_t>("player_id");
		const uint16_t amount = result->getNumber<uint16_t>("amount");
		if (result->getDataInt("sale") == 1) {
			Player* player = g_game.getPlayerByGUID(playerId);
			if (!player) {
				player = new Player(nullptr);
				if (!IOLoginData::loadPlayerById(player, playerId)) {
					delete player;
					continue;
				}
			}

			const ItemType& itemType = Item::items[result->getNumber<uint16_t>("itemtype")];
			if (itemType.id == 0) {
				continue;
			}

			if (itemType.stackable) {
				uint16_t tmpAmount = amount;
				while (tmpAmount > 0) {
					uint16_t stackCount = std::min<uint16_t>(100, tmpAmount);
					Item* item = Item::CreateItem(itemType.id, stackCount);
					if (g_game.internalAddItem(player->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RETURNVALUE_NOERROR) {
						delete item;
						break;
					}

					tmpAmount -= stackCount;
				}
			} else {
				int32_t subType;
				if (itemType.charges != 0) {
					subType = itemType.charges;
				} else {
					subType = -1;
				}

				for (uint16_t i = 0; i < amount; ++i) {
					Item* item = Item::CreateItem(itemType.id, subType);
					if (g_game.internalAddItem(player->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RETURNVALUE_NOERROR) {
						delete item;
						break;
					}
				}
			}

			if (player->isOffline()) {
				IOLoginData::savePlayer(player);
				delete player;
			}
		} else {
			uint64_t totalPrice = result->getNumber<uint64_t>("price") * amount;

			Player* player = g_game.getPlayerByGUID(playerId);
			if (player) {
				player->setBankBalance(player->getBankBalance() + totalPrice);
			} else {
				IOLoginData::increaseBankBalance(playerId, totalPrice);
			}
		}
	} while (result->next());
}

void IOMarket::checkExpiredOffers()
{
	const time_t lastExpireDate = time(nullptr) - g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);

	std::ostringstream query;
	query << "SELECT `id`, `amount`, `price`, `itemtype`, `player_id`, `sale` FROM `market_offers` WHERE `created` <= " << lastExpireDate;
	g_databaseTasks.addTask(query.str(), IOMarket::processExpiredOffers, true);

	int32_t checkExpiredMarketOffersEachMinutes = g_config.getNumber(ConfigManager::CHECK_EXPIRED_MARKET_OFFERS_EACH_MINUTES);
	if (checkExpiredMarketOffersEachMinutes <= 0) {
		return;
	}

	g_scheduler.addEvent(createSchedulerTask(checkExpiredMarketOffersEachMinutes * 60 * 1000, IOMarket::checkExpiredOffers));
}

int32_t IOMarket::getPlayerOfferCount(uint32_t playerId)
{
	std::ostringstream query;
	query << "SELECT COUNT(*) AS `count` FROM `market_offers` WHERE `player_id` = " << playerId;

	DBResult_ptr result = Database::getInstance()->storeQuery(query.str());
	if (!result) {
		return -1;
	}
	return result->getDataInt("count");
}

MarketOfferEx IOMarket::getOfferByCounter(uint32_t timestamp, uint16_t counter)
{
	MarketOfferEx offer;

	const int32_t created = timestamp - g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);

	std::ostringstream query;
	query << "SELECT `id`, `sale`, `itemtype`, `amount`, `created`, `price`, `player_id`, `anonymous`, (SELECT `name` FROM `players` WHERE `id` = `player_id`) AS `player_name` FROM `market_offers` WHERE `created` = " << created << " AND (`id` & 65535) = " << counter << " LIMIT 1";

	DBResult_ptr result = Database::getInstance()->storeQuery(query.str());
	if (!result) {
		offer.id = 0;
		return offer;
	}

	offer.id = result->getDataInt("id");
	offer.type = static_cast<MarketAction_t>(result->getDataInt("sale"));
	offer.amount = result->getDataInt("amount");
	offer.counter = result->getDataInt("id") & 0xFFFF;
	offer.timestamp = result->getDataInt("created");
	offer.price = result->getDataInt("price");
	offer.itemId = result->getDataInt("itemtype");
	offer.playerId = result->getDataInt("player_id");
	if (result->getDataInt("anonymous") == 0) {
		offer.playerName = result->getDataString("player_name");
	} else {
		offer.playerName = "Anonymous";
	}
	return offer;
}

void IOMarket::createOffer(uint32_t playerId, MarketAction_t action, uint32_t itemId, uint16_t amount, uint32_t price, bool anonymous)
{
	std::ostringstream query;
	query << "INSERT INTO `market_offers` (`player_id`, `sale`, `itemtype`, `amount`, `price`, `created`, `anonymous`) VALUES (" << playerId << ',' << action << ',' << itemId << ',' << amount << ',' << price << ',' << time(nullptr) << ',' << anonymous << ')';
	Database::getInstance()->executeQuery(query.str());
}

void IOMarket::acceptOffer(uint32_t offerId, uint16_t amount)
{
	std::ostringstream query;
	query << "UPDATE `market_offers` SET `amount` = `amount` - " << amount << " WHERE `id` = " << offerId;
	Database::getInstance()->executeQuery(query.str());
}

void IOMarket::deleteOffer(uint32_t offerId)
{
	std::ostringstream query;
	query << "DELETE FROM `market_offers` WHERE `id` = " << offerId;
	Database::getInstance()->executeQuery(query.str());
}

void IOMarket::appendHistory(uint32_t playerId, MarketAction_t type, uint16_t itemId, uint16_t amount, uint32_t price, time_t timestamp, MarketOfferState_t state)
{
	std::ostringstream query;
	query << "INSERT INTO `market_history` (`player_id`, `sale`, `itemtype`, `amount`, `price`, `expires_at`, `inserted`, `state`) VALUES ("
		<< playerId << ',' << type << ',' << itemId << ',' << amount << ',' << price << ','
		<< timestamp << ',' << time(nullptr) << ',' << state << ')';
	g_databaseTasks.addTask(query.str());
}

bool IOMarket::moveOfferToHistory(uint32_t offerId, MarketOfferState_t state)
{
	const int32_t marketOfferDuration = g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);

	Database* db = Database::getInstance();

	std::ostringstream query;
	query << "SELECT `player_id`, `sale`, `itemtype`, `amount`, `price`, `created` FROM `market_offers` WHERE `id` = " << offerId;

	DBResult_ptr result = db->storeQuery(query.str());
	if (!result) {
		return false;
	}

	query.str("");
	query << "DELETE FROM `market_offers` WHERE `id` = " << offerId;
	if (!db->executeQuery(query.str())) {
		return false;
	}

	appendHistory(result->getDataInt("player_id"), static_cast<MarketAction_t>(result->getDataInt("sale")), result->getDataInt("itemtype"), result->getDataInt("amount"), result->getDataInt("price"), result->getDataInt("created") + marketOfferDuration, state);
	return true;
}

void IOMarket::updateStatistics()
{
	std::ostringstream query;
	query << "SELECT `sale` AS `sale`, `itemtype` AS `itemtype`, COUNT(`price`) AS `num`, MIN(`price`) AS `min`, MAX(`price`) AS `max`, SUM(`price`) AS `sum` FROM `market_history` WHERE `state` = " << OFFERSTATE_ACCEPTED << " GROUP BY `itemtype`, `sale`";
	DBResult_ptr result = Database::getInstance()->storeQuery(query.str());
	if (!result) {
		return;
	}

	do {
		MarketStatistics* statistics;
		if (result->getDataInt("sale") == MARKETACTION_BUY) {
			statistics = &purchaseStatistics[result->getDataInt("itemtype")];
		} else {
			statistics = &saleStatistics[result->getDataInt("itemtype")];
		}

		statistics->numTransactions = result->getDataInt("num");
		statistics->lowestPrice = result->getDataInt("min");
		statistics->totalPrice = result->getNumber<uint64_t>("sum");
		statistics->highestPrice = result->getDataInt("max");
	} while (result->next());
}

MarketStatistics* IOMarket::getPurchaseStatistics(uint16_t itemId)
{
	auto it = purchaseStatistics.find(itemId);
	if (it == purchaseStatistics.end()) {
		return nullptr;
	}
	return &it->second;
}

MarketStatistics* IOMarket::getSaleStatistics(uint16_t itemId)
{
	auto it = saleStatistics.find(itemId);
	if (it == saleStatistics.end()) {
		return nullptr;
	}
	return &it->second;
}
