/*
 * Copyright (c) 2023  Gaurav Ujjwal.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

package com.gaurav.avnc.model

import android.content.ContentValues
import android.database.sqlite.SQLiteDatabase
import androidx.room.testing.MigrationTestHelper
import com.gaurav.avnc.instrumentation
import com.gaurav.avnc.model.db.MainDb
import com.gaurav.avnc.model.db.ProfileSecrets
import com.gaurav.avnc.targetContext
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test

class DatabaseTest {
    private val dbName = "Bond. James Bond."
    private val minVersion = 1
    private val maxVersion = MainDb.VERSION

    @get:Rule
    val helper = MigrationTestHelper(instrumentation, MainDb::class.java,
            listOf(MainDb.EncryptCredentials(ProfileSecrets(targetContext))))

    @Test
    fun existingCredentialsCanBeReadWithTheDeviceKeyAfterUpgrade() {
        val columns = listOf("password", "sshPassword", "sshPrivateKey")
        helper.createDatabase(dbName, 8).use { db ->
            val row = ContentValues()
            db.query("PRAGMA table_info(profiles)").use { fields ->
                while (fields.moveToNext()) {
                    val name = fields.getString(1)
                    if (fields.getString(2) == "TEXT") row.put(name, "") else row.put(name, 0)
                }
            }
            row.put("ID", 7)
            columns.forEach { row.put(it, "fixture-$it") }
            db.insert("profiles", SQLiteDatabase.CONFLICT_ABORT, row)
        }
        helper.runMigrationsAndValidate(dbName, 9, false).use { db ->
            db.query("SELECT password, sshPassword, sshPrivateKey FROM profiles WHERE ID=7").use { row ->
                assertTrue(row.moveToFirst())
                val secrets = ProfileSecrets(targetContext)
                columns.forEachIndexed { index, field ->
                    assertEquals(android.database.Cursor.FIELD_TYPE_BLOB, row.getType(index))
                    assertEquals("fixture-$field", secrets.decrypt(row.getBlob(index)))
                }
            }
        }
    }

    @Test
    fun migrations() {
        for (i in minVersion until maxVersion)
            for (j in i + 1..maxVersion)
                runCatching {
                    helper.createDatabase(dbName, i).close()
                    helper.runMigrationsAndValidate(dbName, j, false).close()
                }.onFailure {
                    throw Exception("Failed to migrate MainDb from [$i] to [$j]", it)
                }
    }
}
